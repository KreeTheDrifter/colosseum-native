// UpscaleTextures: builds an AI-upscaled HD texture pack for Colosseum Native from the
// textures the game dumped while you played (menu: "Dump textures for HD pack").
//
//   User\Dump\Textures\GC6E01\tex1_*.png   ->   User\Load\Textures\GC6E01\tex1_*.png (+ _mipN)
//
// Upscaling is done by Real-ESRGAN (ncnn/Vulkan build, BSD-3, xinntao/Real-ESRGAN), which
// runs on any Vulkan GPU including AMD. It's downloaded on first use into tools\realesrgan.
// Colour is upscaled by the model; alpha separately with a Mitchell filter (the models
// aren't trained on transparency), then recombined. A full mip chain is generated so
// distant surfaces don't shimmer. Already-upscaled textures are skipped, so it can be run
// again after playing further.
//
//   UpscaleTextures.exe [--scale 2|4] [--model anime|photo] [--force] [--include-video]
//                       [--root <folder with User\>]   (default: the exe's folder)

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <urlmon.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "third_party/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "third_party/stb_image_resize2.h"

namespace fs = std::filesystem;

namespace
{
constexpr const char* kGameId = "GC6E01";
constexpr const char* kEsrganUrl =
    "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/"
    "realesrgan-ncnn-vulkan-20220424-windows.zip";

struct Image
{
  int w = 0, h = 0;
  std::vector<unsigned char> rgba;  // 4 channels

  bool Load(const fs::path& p)
  {
    int n = 0;
    unsigned char* data = stbi_load(p.string().c_str(), &w, &h, &n, 4);
    if (!data)
      return false;
    rgba.assign(data, data + size_t(w) * h * 4);
    stbi_image_free(data);
    return true;
  }
  bool Save(const fs::path& p) const
  {
    return stbi_write_png(p.string().c_str(), w, h, 4, rgba.data(), w * 4) != 0;
  }
  bool HasAlpha() const
  {
    for (size_t i = 3; i < rgba.size(); i += 4)
      if (rgba[i] != 255)
        return true;
    return false;
  }
};

// alpha_aware: weight colour by alpha (RGBA layout, sRGB colour). Otherwise all four
// channels are resized independently as linear data (used for the alpha-only pass).
Image Resize(const Image& src, int w, int h, bool alpha_aware)
{
  Image out;
  out.w = w;
  out.h = h;
  out.rgba.resize(size_t(w) * h * 4);
  stbir_resize(src.rgba.data(), src.w, src.h, 0, out.rgba.data(), w, h, 0,
               alpha_aware ? STBIR_RGBA : STBIR_4CHANNEL,
               alpha_aware ? STBIR_TYPE_UINT8_SRGB : STBIR_TYPE_UINT8,
               STBIR_EDGE_WRAP, STBIR_FILTER_MITCHELL);
  return out;
}

bool Run(const std::string& cmd)
{
  std::printf("  > %s\n", cmd.c_str());
  std::fflush(stdout);
  return std::system(cmd.c_str()) == 0;
}

fs::path ExeDir(const char* argv0)
{
#ifdef _WIN32
  wchar_t buf[MAX_PATH * 4];
  GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
  return fs::path(buf).parent_path();
#else
  return fs::absolute(argv0).parent_path();
#endif
}

fs::path FindEngine(const fs::path& tools)
{
  for (const char* name : {"realesrgan-ncnn-vulkan.exe", "realesrgan-ncnn-vulkan"})
  {
    if (fs::exists(tools / name))
      return tools / name;
  }
  return {};
}

bool DownloadEngine(const fs::path& tools)
{
#ifdef _WIN32
  fs::create_directories(tools);
  const fs::path zip = tools / "realesrgan.zip";
  std::printf("Downloading Real-ESRGAN (about 45 MB, first run only)...\n");
  std::fflush(stdout);
  if (FAILED(URLDownloadToFileA(nullptr, kEsrganUrl, zip.string().c_str(), 0, nullptr)))
  {
    std::printf("Download failed. You can download it yourself from\n  %s\nand unzip it into %s\n",
                kEsrganUrl, tools.string().c_str());
    return false;
  }
  // Windows 10+ ships bsdtar, which reads zip files.
  const bool ok = Run("tar -xf \"" + zip.string() + "\" -C \"" + tools.string() + "\"");
  fs::remove(zip);
  return ok && !FindEngine(tools).empty();
#else
  std::printf("Put realesrgan-ncnn-vulkan and its models folder in %s\n", tools.string().c_str());
  return false;
#endif
}

// Colosseum streams its FMVs (intro, cutscenes) as 320x240 / 640x480 textures, one per
// frame; those never repeat, so upscaling them only costs time and disk.
bool IsVideoFrame(const fs::path& p)
{
  const std::string name = p.filename().string();
  return name.rfind("tex1_320x240_", 0) == 0 || name.rfind("tex1_640x480_", 0) == 0;
}

bool IsBaseTexture(const fs::path& p)
{
  const std::string name = p.filename().string();
  return p.extension() == ".png" && name.rfind("tex1_", 0) == 0 &&
         name.find("_mip") == std::string::npos;
}
}  // namespace

int Main(int argc, char** argv)
{
  int scale = 4;
  std::string model = "realesrgan-x4plus-anime";
  bool force = false;
  bool include_video = false;
  fs::path root = ExeDir(argv[0]);
  for (int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];
    if (a == "--scale" && i + 1 < argc)
      scale = std::atoi(argv[++i]) == 2 ? 2 : 4;
    else if (a == "--model" && i + 1 < argc)
      model = std::string(argv[++i]) == "photo" ? "realesrgan-x4plus" : "realesrgan-x4plus-anime";
    else if (a == "--force")
      force = true;
    else if (a == "--include-video")
      include_video = true;
    else if (a == "--root" && i + 1 < argc)
      root = argv[++i];
  }

  const fs::path dump = root / "User" / "Dump" / "Textures" / kGameId;
  const fs::path load = root / "User" / "Load" / "Textures" / kGameId;
  const fs::path tools = root / "tools" / "realesrgan";
  const fs::path work = root / "tools" / "upscale-work";

  std::printf("Colosseum Native HD texture builder\n  dumped:  %s\n  pack:    %s\n  scale:   %dx, model %s\n\n",
              dump.string().c_str(), load.string().c_str(), scale, model.c_str());

  std::vector<fs::path> todo;
  size_t skipped_video = 0;
  if (fs::exists(dump))
  {
    for (const auto& e : fs::recursive_directory_iterator(dump))
    {
      if (!e.is_regular_file() || !IsBaseTexture(e.path()))
        continue;
      if (!include_video && IsVideoFrame(e.path()))
      {
        ++skipped_video;
        continue;
      }
      if (!force && fs::exists(load / e.path().filename()))
        continue;
      todo.push_back(e.path());
    }
  }
  if (todo.empty())
  {
    std::printf("Nothing new to upscale. Turn on \"Dump textures for HD pack\" in the game's\n"
                "menu (Esc), play for a while, then run this again.\n");
    return 0;
  }
  std::printf("%zu new textures (%zu movie frames skipped).\n", todo.size(), skipped_video);

  fs::path engine = FindEngine(tools);
  if (engine.empty() && (!DownloadEngine(tools) || (engine = FindEngine(tools)).empty()))
    return 1;

  // Stage colour-only copies (alpha forced opaque) for the model.
  fs::remove_all(work);
  fs::create_directories(work / "in");
  fs::create_directories(work / "out");
  fs::create_directories(load);
  std::vector<Image> sources(todo.size());
  for (size_t i = 0; i < todo.size(); ++i)
  {
    if (!sources[i].Load(todo[i]))
    {
      std::printf("  skipping unreadable %s\n", todo[i].filename().string().c_str());
      continue;
    }
    Image rgb = sources[i];
    for (size_t k = 3; k < rgb.rgba.size(); k += 4)
      rgb.rgba[k] = 255;
    rgb.Save(work / "in" / todo[i].filename());
  }

  const auto start = std::chrono::steady_clock::now();
  std::printf("Upscaling with Real-ESRGAN on your GPU...\n");
  if (!Run("\"" + engine.string() + "\" -i \"" + (work / "in").string() + "\" -o \"" +
           (work / "out").string() + "\" -n " + model + " -s 4 -f png -m \"" +
           (engine.parent_path() / "models").string() + "\""))
  {
    std::printf("Real-ESRGAN failed. Is your graphics driver up to date (Vulkan)?\n");
    return 1;
  }

  size_t done = 0, mips_written = 0;
  for (size_t i = 0; i < todo.size(); ++i)
  {
    const Image& src = sources[i];
    if (src.w == 0)
      continue;
    Image up;
    if (!up.Load(work / "out" / todo[i].filename()) || up.w != src.w * 4 || up.h != src.h * 4)
    {
      std::printf("  no usable output for %s\n", todo[i].filename().string().c_str());
      continue;
    }
    // Alpha: upscale the original separately and put it back.
    if (src.HasAlpha())
    {
      Image alpha = src;
      for (size_t k = 0; k < alpha.rgba.size(); k += 4)
        alpha.rgba[k] = alpha.rgba[k + 1] = alpha.rgba[k + 2] = alpha.rgba[k + 3];
      const Image a4 = Resize(alpha, up.w, up.h, false);
      for (size_t k = 0; k < up.rgba.size(); k += 4)
        up.rgba[k + 3] = a4.rgba[k];
    }
    Image base = scale == 4 ? up : Resize(up, src.w * 2, src.h * 2, true);

    const fs::path out = load / todo[i].filename();
    if (!base.Save(out))
      continue;
    // Mip chain: halve until 1x1 (Dolphin loads tex_mip1, tex_mip2, ... next to the base).
    Image level = base;
    for (int m = 1; level.w > 1 || level.h > 1; ++m)
    {
      level = Resize(level, std::max(1, level.w / 2), std::max(1, level.h / 2), true);
      fs::path mp = out;
      mp.replace_filename(out.stem().string() + "_mip" + std::to_string(m) + ".png");
      level.Save(mp);
      ++mips_written;
    }
    ++done;
    if (done % 50 == 0)
      std::printf("  %zu / %zu\n", done, todo.size());
  }
  fs::remove_all(work);

  const double secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::printf("\nDone: %zu textures (%zu mip levels) in %.0f s.\n"
              "Turn on \"HD texture packs\" in the game's menu (on by default) and restart the game.\n",
              done, mips_written, secs);
  return 0;
}

int main(int argc, char** argv)
{
  const int rc = Main(argc, argv);
#ifdef _WIN32
  // Double-clicked (no arguments): keep the console window open to read the result.
  if (argc == 1)
  {
    std::printf("\nPress Enter to close.");
    std::fflush(stdout);
    std::getchar();
  }
#endif
  return rc;
}
