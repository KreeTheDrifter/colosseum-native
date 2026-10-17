// Colosseum Native launcher (Windows).
//
// First launch: asks for the player's Pokemon Colosseum (USA) disc image
// (ISO, GCM or CISO), checks it is GC6E01 with the supported main.dol, unpacks
// it into Game\ next to the exe, writes default settings, then starts the
// recompiled game. Later launches go straight to the game.
//
// Layout next to this exe:
//   ColosseumNative.exe      this launcher
//   moderngekko-run.exe      ModernGekko runtime
//   gGC6E01_recomp.dll       recompiled game code
//   Game\                    unpacked disc (created here, never shipped)
//   User\                    settings, saves, shader cache

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "dol_patches.h"

namespace fs = std::filesystem;

namespace
{
constexpr char kGameId[] = "GC6E01";
constexpr char kMainDolSha1[] = "870e8b9693ca780782d80f22a6a4572d8ba9458f";
constexpr wchar_t kTitle[] = L"Pok\u00e9mon Colosseum Native";

uint32_t be32(const uint8_t* p)
{
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint32_t le32(const uint8_t* p)
{
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

void Fail(const std::wstring& message)
{
  MessageBoxW(nullptr, message.c_str(), kTitle, MB_OK | MB_ICONERROR);
  ExitProcess(1);
}

// Reads a GameCube disc image as a flat disc, whether it is a plain ISO/GCM or a CISO.
class Disc
{
public:
  bool Open(const fs::path& path)
  {
    m_file = _wfopen(path.c_str(), L"rb");
    if (!m_file)
      return false;
    uint8_t header[0x8000];
    if (fread(header, 1, 8, m_file) == 8 && memcmp(header, "CISO", 4) == 0)
    {
      fseek(m_file, 0, SEEK_SET);
      if (fread(header, 1, sizeof(header), m_file) != sizeof(header))
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
    }
    return true;
  }

  ~Disc()
  {
    if (m_file)
      fclose(m_file);
  }

  bool Read(uint64_t offset, void* out, size_t size)
  {
    auto* dst = static_cast<uint8_t*>(out);
    if (m_map.empty())
    {
      if (_fseeki64(m_file, int64_t(offset), SEEK_SET) != 0)
        return false;
      return fread(dst, 1, size, m_file) == size;
    }
    while (size)
    {
      const uint64_t index = offset / m_block;
      const uint64_t within = offset % m_block;
      const size_t chunk = size_t(std::min<uint64_t>(size, m_block - within));
      if (index >= m_map.size() || m_map[index] == UINT64_MAX)
      {
        memset(dst, 0, chunk);
      }
      else
      {
        if (_fseeki64(m_file, int64_t(m_map[index] + within), SEEK_SET) != 0 ||
            fread(dst, 1, chunk, m_file) != chunk)
          return false;
      }
      dst += chunk;
      offset += chunk;
      size -= chunk;
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
  FILE* m_file = nullptr;
  uint32_t m_block = 0;
  std::vector<uint64_t> m_map;
};

std::string Sha1Hex(const std::vector<uint8_t>& data)
{
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  uint8_t digest[20] = {};
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0)
    return {};
  BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
  BCryptHashData(hash, const_cast<PUCHAR>(data.data()), ULONG(data.size()), 0);
  BCryptFinishHash(hash, digest, sizeof(digest), 0);
  BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(alg, 0);
  char hex[41];
  for (int i = 0; i < 20; ++i)
    snprintf(hex + i * 2, 3, "%02x", digest[i]);
  return hex;
}

// A small always-on-top window that shows unpacking progress.
class Progress
{
public:
  Progress()
  {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ColosseumNativeProgress";
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursorW(nullptr, IDC_WAIT);
    RegisterClassW(&wc);
    const int w = 460, h = 110;
    m_window = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, kTitle,
                               WS_CAPTION | WS_VISIBLE, (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
                               (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h, nullptr, nullptr,
                               wc.hInstance, nullptr);
    m_label = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 16, 20, w - 40, 40,
                              m_window, nullptr, wc.hInstance, nullptr);
    SendMessageW(m_label, WM_SETFONT, WPARAM(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
  }
  ~Progress() { DestroyWindow(m_window); }

  void Set(const std::wstring& text)
  {
    SetWindowTextW(m_label, text.c_str());
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
      DispatchMessageW(&msg);
  }

private:
  HWND m_window = nullptr;
  HWND m_label = nullptr;
};

bool WriteFile(const fs::path& path, const std::vector<uint8_t>& data)
{
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
  return bool(out);
}

void WriteText(const fs::path& path, const std::string& text)
{
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

// Offset of a RAM address inside a DOL image, or SIZE_MAX if no section holds it.
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

enum class DolState { Original, Patched, Unknown };

// The recompiled code was built from main.dol with kDolPatches applied (widescreen,
// 60 fps, memory-card fix); the game must boot that exact DOL.
DolState CheckDol(const std::vector<uint8_t>& dol)
{
  bool all_new = true;
  for (const DolPatch& p : kDolPatches)
  {
    const size_t at = DolOffset(dol, p.address);
    if (at == SIZE_MAX)
      return DolState::Unknown;
    if (be32(&dol[at]) != p.replacement)
      all_new = false;
  }
  if (all_new)
    return DolState::Patched;
  return Sha1Hex(dol) == kMainDolSha1 ? DolState::Original : DolState::Unknown;
}

std::vector<uint8_t> PatchDol(std::vector<uint8_t> dol)
{
  for (const DolPatch& p : kDolPatches)
  {
    const size_t at = DolOffset(dol, p.address);
    dol[at] = uint8_t(p.replacement >> 24);
    dol[at + 1] = uint8_t(p.replacement >> 16);
    dol[at + 2] = uint8_t(p.replacement >> 8);
    dol[at + 3] = uint8_t(p.replacement);
  }
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

// Unpacks the disc into root/sys and root/files, the layout ModernGekko expects.
void Unpack(Disc& disc, const std::vector<uint8_t>& dol, const fs::path& root)
{
  Progress progress;
  progress.Set(L"Unpacking your disc (first launch only)…");

  const fs::path partial = root.wstring() + L".partial";
  fs::remove_all(partial);
  fs::create_directories(partial / "sys");
  fs::create_directories(partial / "files");

  const auto sys_area = disc.Read(0, 0x2460);
  const uint32_t app_size = be32(&sys_area[0x2454]);
  const uint32_t app_trailer = be32(&sys_area[0x2458]);
  const uint32_t fst_offset = be32(&sys_area[0x424]);
  const uint32_t fst_size = be32(&sys_area[0x428]);
  const auto fst = disc.Read(fst_offset, fst_size);
  if (fst.size() < 12)
    Fail(L"The disc's file table could not be read.");

  WriteFile(partial / "sys" / "main.dol", PatchDol(dol));
  WriteFile(partial / "sys" / "boot.bin", {sys_area.begin(), sys_area.begin() + 0x440});
  WriteFile(partial / "sys" / "bi2.bin", {sys_area.begin() + 0x440, sys_area.begin() + 0x2440});
  WriteFile(partial / "sys" / "apploader.img", disc.Read(0x2440, 0x20 + app_size + app_trailer));
  WriteFile(partial / "sys" / "fst.bin", fst);

  const uint32_t count = be32(&fst[8]);
  const size_t strings = size_t(count) * 12;
  auto name = [&](uint32_t i) {
    const uint32_t off = be32(&fst[i * 12]) & 0xFFFFFF;
    const char* s = reinterpret_cast<const char*>(&fst[strings + off]);
    return std::string(s);
  };

  std::vector<std::pair<uint32_t, fs::path>> stack{{count, partial / "files"}};
  std::vector<uint8_t> buffer;
  for (uint32_t i = 1; i < count; ++i)
  {
    while (i >= stack.back().first)
      stack.pop_back();
    const uint8_t* e = &fst[i * 12];
    const fs::path path = stack.back().second / fs::path(std::u8string(reinterpret_cast<const char8_t*>(name(i).c_str())));
    if (e[0])
    {
      fs::create_directories(path);
      stack.emplace_back(be32(e + 8), path);
      continue;
    }
    buffer.resize(be32(e + 8));
    if (!disc.Read(be32(e + 4), buffer.data(), buffer.size()))
      Fail(L"Reading the disc failed while unpacking. Is the file complete?");
    if (!WriteFile(path, buffer))
      Fail(L"Could not write " + path.wstring() + L". Is the drive full?");
    if (i % 20 == 0)
      progress.Set(L"Unpacking your disc (first launch only)…  " +
                   std::to_wstring(i * 100 / count) + L"%");
  }
  fs::remove_all(root);
  fs::rename(partial, root);
}

// Sets key=value under [section] in a Dolphin-style ini, but only when the key is not
// there yet, so later builds can add required settings without undoing user edits.
void EnsureIni(const fs::path& path, const std::string& section, const std::string& key,
               const std::string& value, bool overwrite = false)
{
  std::vector<std::string> lines;
  {
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);)
    {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      lines.push_back(line);
    }
  }
  auto trim = [](std::string t) {
    const auto b = t.find_first_not_of(" \t");
    const auto e = t.find_last_not_of(" \t");
    return b == std::string::npos ? std::string() : t.substr(b, e - b + 1);
  };
  const std::string header = "[" + section + "]";
  size_t insert_at = lines.size();
  bool in_section = false, found_section = false;
  for (size_t i = 0; i < lines.size(); ++i)
  {
    const std::string t = trim(lines[i]);
    if (!t.empty() && t.front() == '[')
    {
      if (in_section)
        break;
      in_section = (t == header);
      if (in_section)
      {
        found_section = true;
        insert_at = i + 1;
      }
      continue;
    }
    if (in_section)
    {
      if (const auto eq = t.find('='); eq != std::string::npos && trim(t.substr(0, eq)) == key)
      {
        if (!overwrite || trim(t.substr(eq + 1)) == value)
          return;  // already set; keep the user's value
        lines[i] = key + " = " + value;
        std::string text;
        for (const auto& line : lines)
          text += line + "\n";
        WriteText(path, text);
        return;
      }
      if (!t.empty())
        insert_at = i + 1;
    }
  }
  const std::string entry = key + " = " + value;
  if (found_section)
    lines.insert(lines.begin() + std::ptrdiff_t(insert_at), entry);
  else
  {
    lines.push_back(header);
    lines.push_back(entry);
  }
  std::string text;
  for (const auto& line : lines)
    text += line + "\n";
  WriteText(path, text);
}

// Launcher state (which versions of our defaults an install has), in
// User\Config\ColosseumNative.ini.
int ReadState(const fs::path& user, const std::string& key)
{
  std::ifstream in(user / "Config" / "ColosseumNative.ini");
  for (std::string line; std::getline(in, line);)
  {
    const auto eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    std::string k = line.substr(0, eq);
    k.erase(k.find_last_not_of(" \t") + 1);
    if (k == key)
      return std::atoi(line.c_str() + eq + 1);
  }
  return 0;
}

void WriteState(const fs::path& user, const std::string& key, int value)
{
  EnsureIni(user / "Config" / "ColosseumNative.ini", "Launcher", key, std::to_string(value), true);
}

// Controller profile for GameCube port 1. Xbox (and other XInput) controllers bind by
// position: GameCube B is on Xbox X, GameCube X on Xbox B. Keyboard keys are bound in the
// same profile through device-qualified controls, so either works at any time.
// Version of the default profile below. Dolphin rewrites GCPadNew.ini on exit (dropping
// comments), so the installed version is kept in User\Config\ColosseumNative.ini.
constexpr int kPadProfileVersion = 1;

void WriteControllerProfile(const fs::path& user)
{
  const fs::path path = user / "Config" / "GCPadNew.ini";
  const int installed = ReadState(user, "PadProfile");
  {
    // Replace a missing or empty profile, or our own older default; keep anything else.
    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const bool configured = text.find("Device =") != std::string::npos;
    if (configured && (installed == 0 || installed >= kPadProfileVersion))
      return;
  }
  auto key = [](const char* k) {
    return std::string("`DInput/0/Keyboard Mouse:") + k + "`";
  };
  auto both = [&](const char* pad, const char* k) {
    return std::string("`") + pad + "` | " + key(k);
  };
  std::string p;
  p += "[GCPad1]\n";
  p += "Device = XInput/0/Gamepad\n";
  p += "Buttons/A = " + both("Button A", "X") + "\n";
  p += "Buttons/B = " + both("Button X", "C") + "\n";
  p += "Buttons/X = " + both("Button B", "S") + "\n";
  p += "Buttons/Y = " + both("Button Y", "D") + "\n";
  p += "Buttons/Z = " + both("Shoulder R", "Z") + "\n";
  p += "Buttons/Start = " + both("Start", "RETURN") + "\n";
  p += "Main Stick/Up = " + both("Left Y+", "UP") + "\n";
  p += "Main Stick/Down = " + both("Left Y-", "DOWN") + "\n";
  p += "Main Stick/Left = " + both("Left X-", "LEFT") + "\n";
  p += "Main Stick/Right = " + both("Left X+", "RIGHT") + "\n";
  p += "Main Stick/Modifier = " + key("LSHIFT") + "\n";
  p += "Main Stick/Modifier/Range = 50.0\n";
  p += "Main Stick/Dead Zone = 10.0\n";
  p += "C-Stick/Up = " + both("Right Y+", "I") + "\n";
  p += "C-Stick/Down = " + both("Right Y-", "K") + "\n";
  p += "C-Stick/Left = " + both("Right X-", "J") + "\n";
  p += "C-Stick/Right = " + both("Right X+", "L") + "\n";
  p += "C-Stick/Dead Zone = 10.0\n";
  p += "Triggers/L = " + both("Trigger L", "Q") + "\n";
  p += "Triggers/R = " + both("Trigger R", "W") + "\n";
  p += "Triggers/L-Analog = " + both("Trigger L", "Q") + "\n";
  p += "Triggers/R-Analog = " + both("Trigger R", "W") + "\n";
  p += "Triggers/Threshold = 90.0\n";
  p += "D-Pad/Up = " + both("Pad N", "T") + "\n";
  p += "D-Pad/Down = " + both("Pad S", "G") + "\n";
  p += "D-Pad/Left = " + both("Pad W", "F") + "\n";
  p += "D-Pad/Right = " + both("Pad E", "H") + "\n";
  p += "Rumble/Motor = `Motor L` | `Motor R`\n";
  p += "[GCPad2]\n[GCPad3]\n[GCPad4]\n";
  WriteText(path, p);
  WriteState(user, "PadProfile", kPadProfileVersion);
}

// Default settings. Whole files are written only when missing; required keys are added
// to existing files without touching anything the player changed.
void WriteDefaultSettings(const fs::path& user)
{
  fs::create_directories(user / "Config");
  fs::create_directories(user / "GC");
  if (!fs::exists(user / "config.ini"))
  {
    // Vulkan is fastest; fall back to OpenGL on PCs without a Vulkan driver.
    const HMODULE vulkan = LoadLibraryW(L"vulkan-1.dll");
    const char* backend = vulkan ? "Vulkan" : "OGL";
    if (vulkan)
      FreeLibrary(vulkan);
    WriteText(user / "config.ini",
              "# ModernGekko frontend settings\n"
              "# resolution is the internal render size: 640x528 times 1-12.\n"
              "[Video]\n"
              "resolution=2560x2112\n"
              "backend=" + std::string(backend) + "\n"
              "fullscreen=false\n"
              "show_fps_in_title=true\n"
              "[Input]\n");
  }

  const fs::path gfx = user / "Config" / "GFX.ini";
  if (!fs::exists(gfx))
  {
    WriteText(gfx,
              "[Settings]\n"
              "AspectRatio = 1\n"
              "wideScreenHack = False\n"
              "MSAA = 0x00000004\n"
              "SSAA = False\n"
              "EnablePixelLighting = True\n"
              "ShaderCompilationMode = 2\n"
              "WaitForShadersBeforeStarting = False\n"
              "HiresTextures = True\n"
              "[Enhancements]\n"
              "MaxAnisotropy = 4\n"
              "ArbitraryMipmapDetection = True\n"
              "ForceTrueColor = True\n"
              "DisableCopyFilter = True\n");
  }
  // From Dolphin's GC6E01 game settings: the game rewrites its font texture in place,
  // so the fast texture cache shows stale or garbled text; and CPU culling speeds up
  // Shadow Pokemon cutscenes.
  EnsureIni(gfx, "Settings", "SafeTextureCacheColorSamples", "0");
  EnsureIni(gfx, "Settings", "CPUCull", "True");

  // Graphics defaults v2: the game itself now renders 16:9 (patched into the
  // recompiled code), so lock the output to 16:9 and turn off Dolphin's widescreen
  // hack, whose automatic 4:3/16:9 detection flipped between areas. Applied once
  // to installs made with an older default.
  constexpr int kGfxDefaultsVersion = 2;
  if (ReadState(user, "GfxDefaults") < kGfxDefaultsVersion)
  {
    EnsureIni(gfx, "Settings", "AspectRatio", "1", true);
    EnsureIni(gfx, "Settings", "wideScreenHack", "False", true);
    WriteState(user, "GfxDefaults", kGfxDefaultsVersion);
  }

  WriteControllerProfile(user);

  // Memory card: a standard formatted card file in User\GC (Dolphin .raw format).
  const fs::path dolphin = user / "Config" / "Dolphin.ini";
  EnsureIni(dolphin, "Core", "SlotA", "1");
  EnsureIni(dolphin, "Core", "SlotB", "255");
  EnsureIni(dolphin, "Core", "MemcardAPath", (user / "GC" / "MemoryCardA.USA.raw").string());
}

fs::path PickDisc()
{
  wchar_t file[MAX_PATH * 4] = L"";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.lpstrFilter = L"GameCube disc image (*.iso, *.gcm, *.ciso)\0*.iso;*.gcm;*.ciso\0All files\0*.*\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = DWORD(std::size(file));
  ofn.lpstrTitle = L"Choose your Pok\u00e9mon Colosseum (USA) disc image";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
  if (!GetOpenFileNameW(&ofn))
    return {};
  return file;
}

bool GameReady(const fs::path& game)
{
  std::error_code ec;
  return fs::is_regular_file(game / "sys" / "main.dol", ec) &&
         fs::is_regular_file(game / "sys" / "boot.bin", ec) &&
         fs::is_directory(game / "files", ec);
}

void SetupGame(const fs::path& game, fs::path disc_path)
{
  if (disc_path.empty())
  {
    MessageBoxW(nullptr,
                L"Welcome! To play, choose your own copy of Pok\u00e9mon Colosseum for "
                L"GameCube (USA). ISO, GCM and CISO files all work.\n\n"
                L"This only happens once. (You can also drag the file onto "
                L"ColosseumNative.exe.)",
                kTitle, MB_OK | MB_ICONINFORMATION);
    disc_path = PickDisc();
    if (disc_path.empty())
      ExitProcess(0);
  }

  Disc disc;
  if (!disc.Open(disc_path))
    Fail(L"That file could not be opened.");
  const auto id = disc.Read(0, 6);
  if (id.size() != 6 || memcmp(id.data(), kGameId, 6) != 0)
  {
    const std::string got(id.begin(), id.end());
    Fail(L"That isn't Pok\u00e9mon Colosseum (USA). Its game ID is \"" +
         std::wstring(got.begin(), got.end()) + L"\", but this app needs GC6E01.");
  }
  const auto dol = ReadDol(disc);
  if (Sha1Hex(dol) != kMainDolSha1)
    Fail(L"This disc is Pok\u00e9mon Colosseum (USA), but its game program doesn't match "
         L"the supported release. The image may be modified or damaged.");
  Unpack(disc, dol, game);
}
}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
  wchar_t exe[MAX_PATH * 4];
  GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
  const fs::path here = fs::path(exe).parent_path();
  const fs::path runtime = here / "moderngekko-run.exe";
  const fs::path module = here / "gGC6E01_recomp.dll";
  const fs::path game = here / "Game";
  const fs::path user = here / "User";
  if (!fs::is_directory(here / "Sys"))
    Fail(L"The Sys folder is missing. Keep all files from the download together in one folder.");

  if (!fs::exists(runtime) || !fs::exists(module))
    Fail(L"moderngekko-run.exe or gGC6E01_recomp.dll is missing. Keep all files from "
         L"the download together in one folder.");

  // Arguments: an optional disc image path (also what drag-and-drop passes), and
  // --setup-only to unpack and write settings without starting the game.
  fs::path disc_arg;
  bool setup_only = false;
  int argc = 0;
  if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc))
  {
    for (int i = 1; i < argc; ++i)
    {
      if (std::wstring(argv[i]) == L"--setup-only")
        setup_only = true;
      else
        disc_arg = argv[i];
    }
    LocalFree(argv);
  }

  if (!GameReady(game) || !disc_arg.empty())
    SetupGame(game, disc_arg);

  // Installs unpacked by an older build hold the original main.dol; patch it in place.
  {
    const fs::path dol_path = game / "sys" / "main.dol";
    std::ifstream in(dol_path, std::ios::binary);
    std::vector<uint8_t> dol((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    switch (CheckDol(dol))
    {
    case DolState::Patched:
      break;
    case DolState::Original:
      if (!WriteFile(dol_path, PatchDol(std::move(dol))))
        Fail(L"Could not update Game\\sys\\main.dol. Is the folder read-only?");
      break;
    case DolState::Unknown:
      Fail(L"Game\\sys\\main.dol doesn't match this build. Delete the Game folder and run "
           L"ColosseumNative.exe again to unpack your disc.");
    }
  }
  WriteDefaultSettings(user);
  if (setup_only)
    return 0;

  std::wstring cmd = L"\"" + runtime.wstring() + L"\" --game \"" + game.wstring() +
                     L"\" --module \"" + module.wstring() + L"\" --user-dir \"" +
                     user.wstring() + L"\" --title \"Pok\u00e9mon Colosseum\"";
  // The runtime is a console program: hide its console and keep its output in a log.
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  const fs::path log_path = user / "ColosseumNative.log";
  HANDLE log = CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  if (log != INVALID_HANDLE_VALUE)
  {
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = log;
    si.hStdError = log;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  }
  PROCESS_INFORMATION pi{};
  const BOOL started = CreateProcessW(runtime.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr, here.c_str(), &si, &pi);
  if (log != INVALID_HANDLE_VALUE)
    CloseHandle(log);
  if (!started)
    Fail(L"The game could not be started (error " + std::to_wstring(GetLastError()) + L").");
  CloseHandle(pi.hThread);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  return int(code);
}
