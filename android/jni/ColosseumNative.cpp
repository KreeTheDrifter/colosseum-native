// Colosseum Native for Android: the native half of the launcher and the game hooks.
//
//  - Setup: reads the player's Pokemon Colosseum (USA) disc image (ISO, GCM or CISO, opened
//    by the app through Android's file picker and passed in as a file descriptor), checks it
//    is GC6E01 with the supported main.dol, and unpacks it into the app's Game folder.
//  - Before each launch: writes first-run settings and patches main.dol to match the
//    recompiled code (the same patches and settings block as the Windows launcher).
//  - Boot: runs the game on the StaticRecomp CPU core with the recompiled code
//    (libgGC6E01_recomp.so, installed with the app) and adds the in-game menu.
//
// The Kotlin side is org.dolphinemu.dolphinemu.colosseum.ColosseumNative.

#include "ColosseumNative.h"

#include <jni.h>
#include <unistd.h>

#include <android/log.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "jni/AndroidCommon/AndroidCommon.h"
#include "jni/AndroidCommon/IDCache.h"
#include "Common/Config/Config.h"
#include "Common/Crypto/SHA1.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/StaticRecompSettings.h"
#include "Core/Core.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "VideoCommon/OnScreenUI.h"
#include "VideoCommon/VideoConfig.h"

#include "dol_patches.h"
#include "game_menu.hpp"

namespace fs = std::filesystem;

namespace
{
constexpr char kTag[] = "ColosseumNative";
constexpr char kGameId[] = "GC6E01";
constexpr char kMainDolSha1[] = "870e8b9693ca780782d80f22a6a4572d8ba9458f";

std::atomic<int> s_setup_progress{0};
std::mutex s_mutex;
std::string s_module_path;
std::string s_gpu_driver_name;
jclass s_colosseum_class = nullptr;  // global ref; FindClass can't see app classes off the main thread

uint32_t be32(const uint8_t* p)
{
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint32_t le32(const uint8_t* p)
{
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// A GameCube disc image as a flat disc, plain ISO/GCM or CISO.
class Disc
{
public:
  explicit Disc(int fd) : m_fd(fd) {}
  ~Disc() { close(m_fd); }

  bool Open()
  {
    uint8_t header[0x8000];
    if (pread(m_fd, header, 8, 0) != 8)
      return false;
    if (memcmp(header, "CISO", 4) != 0)
      return true;
    if (pread(m_fd, header, sizeof(header), 0) != static_cast<ssize_t>(sizeof(header)))
      return false;
    m_block = le32(header + 4);
    if (m_block == 0)
      return false;
    uint64_t next = sizeof(header);
    for (size_t i = 8; i < sizeof(header); ++i)
    {
      if (header[i])
      {
        m_map.push_back(next);
        next += m_block;
      }
      else
      {
        m_map.push_back(UINT64_MAX);
      }
    }
    return true;
  }

  bool Read(uint64_t offset, void* out, size_t size)
  {
    auto* dst = static_cast<uint8_t*>(out);
    while (size)
    {
      size_t chunk = size;
      uint64_t at = offset;
      if (!m_map.empty())
      {
        const uint64_t index = offset / m_block;
        const uint64_t within = offset % m_block;
        chunk = size_t(std::min<uint64_t>(size, m_block - within));
        if (index >= m_map.size() || m_map[index] == UINT64_MAX)
        {
          memset(dst, 0, chunk);
          dst += chunk;
          offset += chunk;
          size -= chunk;
          continue;
        }
        at = m_map[index] + within;
      }
      const ssize_t got = pread(m_fd, dst, chunk, static_cast<off_t>(at));
      if (got <= 0)
        return false;
      dst += got;
      offset += uint64_t(got);
      size -= size_t(got);
    }
    return true;
  }

  std::vector<uint8_t> Read(uint64_t offset, size_t size)
  {
    std::vector<uint8_t> data(size);
    if (!Read(offset, data.data(), size))
      data.clear();
    return data;
  }

private:
  int m_fd;
  uint32_t m_block = 0;
  std::vector<uint64_t> m_map;
};

std::string Sha1Hex(const std::vector<uint8_t>& data)
{
  // Lowercase, like kMainDolSha1 (Dolphin's DigestToString is uppercase).
  std::string hex = Common::SHA1::DigestToString(Common::SHA1::CalculateDigest(data));
  std::transform(hex.begin(), hex.end(), hex.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return hex;
}

bool WriteFile(const fs::path& path, const std::vector<uint8_t>& data)
{
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
  return bool(out);
}

std::vector<uint8_t> ReadFile(const fs::path& path)
{
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

size_t DolOffset(const std::vector<uint8_t>& dol, uint32_t address)
{
  if (dol.size() < 0x100)
    return SIZE_MAX;
  for (int i = 0; i < 18; ++i)
  {
    const uint32_t off = be32(&dol[i * 4]);
    const uint32_t addr = be32(&dol[0x48 + i * 4]);
    const uint32_t size = be32(&dol[0x90 + i * 4]);
    if (size && address >= addr && address - addr < size && off + (address - addr) + 4 <= dol.size())
      return off + (address - addr);
  }
  return SIZE_MAX;
}

void Put32(std::vector<uint8_t>& d, size_t at, uint32_t v)
{
  d[at] = uint8_t(v >> 24);
  d[at + 1] = uint8_t(v >> 16);
  d[at + 2] = uint8_t(v >> 8);
  d[at + 3] = uint8_t(v);
}

// Undo our patches (see the Windows launcher): a genuine USA main.dol, patched by any build
// or not, comes back to kMainDolSha1.
std::vector<uint8_t> RestoreOriginalDol(std::vector<uint8_t> dol)
{
  const size_t settings = DolOffset(dol, kSettingsAddr);
  if (settings == SIZE_MAX || DolOffset(dol, kSettingsAddr + kSettingsSize - 4) == SIZE_MAX)
    return {};
  std::fill(dol.begin() + std::ptrdiff_t(settings),
            dol.begin() + std::ptrdiff_t(settings + kSettingsSize), uint8_t(0));
  for (const DolPatch& p : kDolPatches)
  {
    const size_t at = DolOffset(dol, p.address);
    if (at == SIZE_MAX)
      return {};
    Put32(dol, at, p.original);
  }
  return dol;
}

std::vector<uint8_t> PatchDol(std::vector<uint8_t> dol, bool fps60, float aspect_multiplier)
{
  for (const DolPatch& p : kDolPatches)
    Put32(dol, DolOffset(dol, p.address), p.replacement);
  const size_t at = DolOffset(dol, kSettingsAddr);
  dol[at] = fps60 ? kFpsMask60 : kFpsMaskOriginal;
  uint32_t bits;
  memcpy(&bits, &aspect_multiplier, 4);
  Put32(dol, at + 4, bits);
  return dol;
}

std::vector<uint8_t> ReadDol(Disc& disc)
{
  const auto ptr = disc.Read(0x420, 4);
  if (ptr.size() != 4)
    return {};
  const uint32_t dol_offset = be32(ptr.data());
  const auto header = disc.Read(dol_offset, 0x100);
  if (header.size() != 0x100)
    return {};
  uint32_t size = 0;
  for (int i = 0; i < 18; ++i)
    size = std::max(size, be32(&header[i * 4]) + be32(&header[0x90 + i * 4]));
  return disc.Read(dol_offset, size);
}

// Unpacks the disc into root/sys and root/files. Returns an error message or "".
std::string Unpack(Disc& disc, const std::vector<uint8_t>& dol, const fs::path& root)
{
  std::error_code ec;
  const fs::path partial = root.string() + ".partial";
  fs::remove_all(partial, ec);
  fs::create_directories(partial / "sys", ec);
  fs::create_directories(partial / "files", ec);

  const auto sys_area = disc.Read(0, 0x2460);
  if (sys_area.size() != 0x2460)
    return "The disc's header could not be read.";
  const uint32_t app_size = be32(&sys_area[0x2454]);
  const uint32_t app_trailer = be32(&sys_area[0x2458]);
  const uint32_t fst_offset = be32(&sys_area[0x424]);
  const uint32_t fst_size = be32(&sys_area[0x428]);
  const auto fst = disc.Read(fst_offset, fst_size);
  if (fst.size() < 12)
    return "The disc's file table could not be read.";

  bool ok = WriteFile(partial / "sys" / "main.dol", dol);
  ok &= WriteFile(partial / "sys" / "boot.bin", {sys_area.begin(), sys_area.begin() + 0x440});
  ok &= WriteFile(partial / "sys" / "bi2.bin", {sys_area.begin() + 0x440, sys_area.begin() + 0x2440});
  ok &= WriteFile(partial / "sys" / "apploader.img",
                  disc.Read(0x2440, 0x20 + app_size + app_trailer));
  ok &= WriteFile(partial / "sys" / "fst.bin", fst);
  if (!ok)
    return "Could not write the game files. Is the phone's storage full?";

  const uint32_t count = be32(&fst[8]);
  const size_t strings = size_t(count) * 12;
  if (strings > fst.size())
    return "The disc's file table is damaged.";
  auto name = [&](uint32_t i) {
    const uint32_t off = be32(&fst[i * 12]) & 0xFFFFFF;
    if (strings + off >= fst.size())
      return std::string("_");
    return std::string(reinterpret_cast<const char*>(&fst[strings + off]));
  };

  std::vector<std::pair<uint32_t, fs::path>> stack{{count, partial / "files"}};
  std::vector<uint8_t> buffer;
  for (uint32_t i = 1; i < count; ++i)
  {
    while (i >= stack.back().first)
      stack.pop_back();
    const uint8_t* e = &fst[i * 12];
    const fs::path path = stack.back().second / name(i);
    if (e[0])
    {
      fs::create_directories(path, ec);
      stack.emplace_back(be32(e + 8), path);
      continue;
    }
    buffer.resize(be32(e + 8));
    if (!disc.Read(be32(e + 4), buffer.data(), buffer.size()))
      return "Reading the disc failed while unpacking. Is the file complete?";
    if (!WriteFile(path, buffer))
      return "Could not write the game files. Is the phone's storage full?";
    s_setup_progress = int(uint64_t(i) * 100 / count);
  }
  fs::remove_all(root, ec);
  fs::rename(partial, root, ec);
  if (ec)
    return "Could not finish unpacking (" + ec.message() + ").";
  return {};
}

std::string SetupGame(int fd, const fs::path& game)
{
  s_setup_progress = 0;
  Disc disc(fd);
  if (!disc.Open())
    return "That file could not be read.";
  const auto id = disc.Read(0, 6);
  if (id.size() != 6 || memcmp(id.data(), kGameId, 6) != 0)
  {
    const std::string got(id.begin(), id.end());
    return "That isn't Pokémon Colosseum (USA). Its game ID is \"" + got +
           "\", but this app needs GC6E01.";
  }
  const auto dol = ReadDol(disc);
  if (Sha1Hex(dol) != kMainDolSha1)
  {
    return "This disc is Pokémon Colosseum (USA), but its game program doesn't match the "
           "supported release. The image may be modified or damaged.";
  }
  return Unpack(disc, dol, game);
}

bool GameReady(const fs::path& game)
{
  std::error_code ec;
  return fs::is_regular_file(game / "sys" / "main.dol", ec) &&
         fs::is_regular_file(game / "sys" / "boot.bin", ec) && fs::is_directory(game / "files", ec);
}

std::string GameOption(const std::string& key, const std::string& fallback)
{
  Common::IniFile ini;
  ini.Load(File::GetUserPath(D_CONFIG_IDX) + "ColosseumNative.ini");
  std::string value;
  ini.GetOrCreateSection("Game")->Get(key, &value, fallback);
  return value;
}

// First-run settings for phones, and the ones Colosseum always needs. Saved to Dolphin's
// config files so the settings screens show them.
void WriteDefaultSettings()
{
  const std::string state_path = File::GetUserPath(D_CONFIG_IDX) + "ColosseumNative.ini";
  Common::IniFile state;
  state.Load(state_path);
  auto* launcher = state.GetOrCreateSection("Launcher");
  int defaults = 0;
  launcher->Get("AndroidDefaults", &defaults, 0);
  if (defaults < 1)
  {
    // Balanced: 3x (1920x1584), 2x MSAA, 8x anisotropic, per-pixel lighting. Snapdragon 8
    // Gen 2/3 phones run it at 60; the in-game menu has presets to change it.
    Config::SetBase(Config::GFX_EFB_SCALE, 3);
    Config::SetBase(Config::GFX_MSAA, 2u);
    Config::SetBase(Config::GFX_SSAA, false);
    Config::SetBase(Config::GFX_ENHANCE_MAX_ANISOTROPY, AnisotropicFilteringMode::Force8x);
    Config::SetBase(Config::GFX_ENABLE_PIXEL_LIGHTING, true);
    Config::SetBase(Config::GFX_HIRES_TEXTURES, true);
    Config::SetBase(Config::GFX_ENHANCE_FORCE_TRUE_COLOR, true);
    Config::SetBase(Config::GFX_ENHANCE_DISABLE_COPY_FILTER, true);
    Config::SetBase(Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION, true);
    Config::SetBase(Config::GFX_SHADER_COMPILATION_MODE, ShaderCompilationMode::AsynchronousUberShaders);
    Config::SetBase(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING, true);
    launcher->Set("AndroidDefaults", 1);
  }
  if (defaults < 2)
  {
    // Vulkan: faster on Adreno than OpenGL ES (whose driver rejected some of Dolphin's
    // shaders on an S24 Ultra), and custom GPU drivers only work with it. A shader that
    // fails to build shouldn't stop the game with a dialog either; it's logged instead.
    Config::SetBase(Config::MAIN_GFX_BACKEND, std::string("Vulkan"));
    Config::SetBase(Config::MAIN_USE_PANIC_HANDLERS, false);
    launcher->Set("AndroidDefaults", 2);
  }
  if (defaults < 3)
  {
    // Smoother phone defaults (test 3 was choppy on an S24 Ultra at 3x / 2x MSAA with hybrid
    // ubershaders): 2x internal resolution (1280x1056, sharper than the phone needs at
    // 16:9), no MSAA, 4x anisotropic, vertex lighting, and specialized shaders built once
    // and cached (a short hitch the first time an effect appears, then none). The FPS
    // counter is on so the frame rate can be reported; the menu turns it off.
    Config::SetBase(Config::GFX_EFB_SCALE, 2);
    Config::SetBase(Config::GFX_MSAA, 1u);
    Config::SetBase(Config::GFX_SSAA, false);
    Config::SetBase(Config::GFX_ENHANCE_MAX_ANISOTROPY, AnisotropicFilteringMode::Force4x);
    Config::SetBase(Config::GFX_ENABLE_PIXEL_LIGHTING, false);
    Config::SetBase(Config::GFX_SHADER_COMPILATION_MODE, ShaderCompilationMode::Synchronous);
    Config::SetBase(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING, true);
    Config::SetBase(Config::GFX_SHOW_FPS, true);
    launcher->Set("AndroidDefaults", 3);
  }
  // Idle-loop skip (see the Windows launcher): the game's idle thread at 0x800FEB9C.
  Config::SetBase(Config::MAIN_STATICRECOMP_IDLE_PC, 0x800FEB9Cu);
  // The game renders 16:9 itself (patched into the recompiled code).
  Config::SetBase(Config::GFX_WIDESCREEN_HACK, false);
  const std::string aspect = GameOption("AspectRatio", "16:9");
  if (aspect == "4:3")
  {
    Config::SetBase(Config::GFX_ASPECT_RATIO, AspectMode::ForceStandard);
  }
  else if (aspect == "16:9")
  {
    Config::SetBase(Config::GFX_ASPECT_RATIO, AspectMode::ForceWide);
  }
  else
  {
    const auto colon = aspect.find(':');
    Config::SetBase(Config::GFX_ASPECT_RATIO, AspectMode::Custom);
    Config::SetBase(Config::GFX_CUSTOM_ASPECT_RATIO_WIDTH, std::stoi(aspect.substr(0, colon)));
    Config::SetBase(Config::GFX_CUSTOM_ASPECT_RATIO_HEIGHT, std::stoi(aspect.substr(colon + 1)));
  }
  Config::Save();
  state.Save(state_path);
}

std::string PrepareLaunch(const fs::path& game)
{
  if (!GameReady(game))
    return "The game files are missing. Choose your disc again.";
  WriteDefaultSettings();

  const std::string fps = GameOption("FrameRate", "60");
  const std::string aspect = GameOption("AspectRatio", "16:9");
  float multiplier = 4.0f / 3.0f;
  for (const AspectChoice& a : kAspects)
    if (aspect == a.name)
      multiplier = a.multiplier;

  // The recompiled code was built from main.dol with kDolPatches applied; boot exactly that.
  const fs::path dol_path = game / "sys" / "main.dol";
  const std::vector<uint8_t> dol = ReadFile(dol_path);
  const std::vector<uint8_t> original = RestoreOriginalDol(dol);
  if (original.empty() || Sha1Hex(original) != kMainDolSha1)
    return "The game files don't match this version. Choose your disc again.";
  const std::vector<uint8_t> wanted = PatchDol(original, fps != "30", multiplier);
  if (wanted != dol && !WriteFile(dol_path, wanted))
    return "Could not update the game files.";
  return {};
}

std::string ToString(JNIEnv* env, jstring s)
{
  return GetJString(env, s);
}
}  // namespace

namespace ColosseumNative
{
void BeforeBoot()
{
  std::string module;
  {
    std::lock_guard lock(s_mutex);
    module = s_module_path;
  }
  __android_log_print(ANDROID_LOG_INFO, kTag, "Booting with %s", module.c_str());
  Config::SetCurrent(Config::MAIN_CPU_CORE, PowerPC::CPUCore::StaticRecomp);
  Config::SetCurrent(Config::MAIN_STATICRECOMP_MODULE, true);
  auto& jit = Core::System::GetInstance().GetJitInterface();
  jit.SetStaticRecompModuleSource(StaticRecompModuleSource::Dynamic(module));

  moderngekko::menu::HostHooks hooks;
  hooks.quit = [] {
    JNIEnv* env = IDCache::GetEnvForThread();
    jclass cls = IDCache::GetNativeLibraryClass();
    if (jmethodID stop = env->GetStaticMethodID(cls, "StopEmulation", "()V"))
      env->CallStaticVoidMethod(cls, stop);
  };
  hooks.gpu_driver_name = [] {
    std::lock_guard lock(s_mutex);
    return s_gpu_driver_name.empty() ? std::string("System") : s_gpu_driver_name;
  };
  hooks.change_gpu_driver = [] {
    // Drivers load when the app starts, so this goes back to the start screen to pick one.
    if (!s_colosseum_class)
      return;
    JNIEnv* env = IDCache::GetEnvForThread();
    if (jmethodID m = env->GetStaticMethodID(s_colosseum_class, "onChangeGpuDriver", "()V"))
      env->CallStaticVoidMethod(s_colosseum_class, m);
    if (env->ExceptionCheck())
      env->ExceptionClear();
  };
  moderngekko::menu::SetHostHooks(std::move(hooks));
  VideoCommon::OnScreenUI::SetOverlayHook(moderngekko::menu::Draw);
}

bool MenuIsOpen()
{
  return moderngekko::menu::IsOpen();
}
}  // namespace ColosseumNative

extern "C" {

JNIEXPORT jstring JNICALL Java_org_dolphinemu_dolphinemu_colosseum_ColosseumNative_setupGame(
    JNIEnv* env, jclass, jint fd, jstring game_dir)
{
  const std::string error = SetupGame(fd, ToString(env, game_dir));
  if (!error.empty())
    __android_log_print(ANDROID_LOG_ERROR, kTag, "Setup failed: %s", error.c_str());
  return ToJString(env, error);
}

JNIEXPORT jint JNICALL Java_org_dolphinemu_dolphinemu_colosseum_ColosseumNative_getSetupProgress(
    JNIEnv*, jclass)
{
  return s_setup_progress;
}

JNIEXPORT jboolean JNICALL Java_org_dolphinemu_dolphinemu_colosseum_ColosseumNative_isGameReady(
    JNIEnv* env, jclass, jstring game_dir)
{
  return GameReady(ToString(env, game_dir)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL Java_org_dolphinemu_dolphinemu_colosseum_ColosseumNative_prepareLaunch(
    JNIEnv* env, jclass cls, jstring game_dir, jstring module_path, jstring gpu_driver_name)
{
  if (!s_colosseum_class)
    s_colosseum_class = static_cast<jclass>(env->NewGlobalRef(cls));
  {
    std::lock_guard lock(s_mutex);
    s_module_path = ToString(env, module_path);
    s_gpu_driver_name = ToString(env, gpu_driver_name);
  }
  return ToJString(env, PrepareLaunch(ToString(env, game_dir)));
}

JNIEXPORT jboolean JNICALL Java_org_dolphinemu_dolphinemu_colosseum_ColosseumNative_menuTap(
    JNIEnv*, jclass, jfloat x, jfloat y)
{
  return moderngekko::menu::OnTap(x, y) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_dolphinemu_dolphinemu_colosseum_ColosseumNative_menuToggle(JNIEnv*,
                                                                                         jclass)
{
  moderngekko::menu::Open();
}

JNIEXPORT jboolean JNICALL Java_org_dolphinemu_dolphinemu_colosseum_ColosseumNative_menuIsOpen(
    JNIEnv*, jclass)
{
  return moderngekko::menu::IsOpen() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jobjectArray JNICALL Java_org_dolphinemu_dolphinemu_colosseum_ColosseumNative_menuItems(
    JNIEnv* env, jclass)
{
  // One string per row: "<h|a|s>\x1f<label>\x1f<value>" (header, action, setting).
  std::vector<std::string> rows;
  for (const auto& item : moderngekko::menu::ExportItems())
  {
    const char kind = item.header ? 'h' : item.action ? 'a' : 's';
    rows.push_back(std::string(1, kind) + '\x1f' + item.label + '\x1f' + item.value);
  }
  return SpanToJStringArray(env, rows);
}

JNIEXPORT jstring JNICALL Java_org_dolphinemu_dolphinemu_colosseum_ColosseumNative_menuChange(
    JNIEnv* env, jclass, jint index, jint dir)
{
  return ToJString(env, moderngekko::menu::Change(index, dir));
}

JNIEXPORT void JNICALL Java_org_dolphinemu_dolphinemu_colosseum_ColosseumNative_menuSetPanelOpen(
    JNIEnv*, jclass, jboolean open)
{
  moderngekko::menu::SetExternalOpen(open == JNI_TRUE);
}

}  // extern "C"
