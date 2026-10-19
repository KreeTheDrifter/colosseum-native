#include "disc_setup.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

#include "Common/Crypto/SHA1.h"

#include "dol_patches.h"

namespace fs = std::filesystem;

namespace disc_setup
{
namespace
{
constexpr char kGameId[] = "GC6E01";
constexpr char kMainDolSha1[] = "870e8b9693ca780782d80f22a6a4572d8ba9458f";

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
  explicit Disc(const ReadAt& read) : m_read(read) {}

  bool Open()
  {
    std::vector<uint8_t> header(0x8000);
    if (!m_read(0, header.data(), 8))
      return false;
    if (memcmp(header.data(), "CISO", 4) != 0)
      return true;
    if (!m_read(0, header.data(), header.size()))
      return false;
    m_block = le32(&header[4]);
    if (m_block == 0)
      return false;
    uint64_t next = header.size();
    for (size_t i = 8; i < header.size(); ++i)
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
      if (!m_read(at, dst, chunk))
        return false;
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
  const ReadAt& m_read;
  uint32_t m_block = 0;
  std::vector<uint64_t> m_map;
};

std::string Sha1Hex(const std::vector<uint8_t>& data)
{
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

std::string Unpack(Disc& disc, const std::vector<uint8_t>& dol, const fs::path& root,
                   std::atomic<int>& progress)
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
    return "Could not write the game files. Is the console's storage full?";

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
      return "Could not write the game files. Is the console's storage full?";
    progress = int(uint64_t(i) * 100 / count);
  }
  fs::remove_all(root, ec);
  fs::rename(partial, root, ec);
  if (ec)
    return "Could not finish unpacking (" + ec.message() + ").";
  progress = 100;
  return {};
}
}  // namespace

std::string SetupGame(const ReadAt& read, const fs::path& game, std::atomic<int>& progress)
{
  progress = 0;
  Disc disc(read);
  if (!disc.Open())
    return "That file could not be read.";
  const auto id = disc.Read(0, 6);
  if (id.size() != 6 || memcmp(id.data(), kGameId, 6) != 0)
  {
    std::string got(id.begin(), id.end());
    for (char& c : got)
      if (c < 0x20 || c > 0x7E)
        c = '?';
    return "That isn't Pokemon Colosseum (USA). Its game ID is \"" + got +
           "\", but this app needs GC6E01.";
  }
  const auto dol = ReadDol(disc);
  if (Sha1Hex(dol) != kMainDolSha1)
  {
    return "This disc is Pokemon Colosseum (USA), but its game program doesn't match the "
           "supported release. The image may be modified or damaged.";
  }
  return Unpack(disc, dol, game, progress);
}

bool GameReady(const fs::path& game)
{
  std::error_code ec;
  return fs::is_regular_file(game / "sys" / "main.dol", ec) &&
         fs::is_regular_file(game / "sys" / "boot.bin", ec) && fs::is_directory(game / "files", ec);
}

std::string PrepareDol(const fs::path& game, bool fps60, const std::string& aspect)
{
  if (!GameReady(game))
    return "The game files are missing.";
  float multiplier = 4.0f / 3.0f;
  for (const AspectChoice& a : kAspects)
    if (aspect == a.name)
      multiplier = a.multiplier;
  const fs::path dol_path = game / "sys" / "main.dol";
  const std::vector<uint8_t> dol = ReadFile(dol_path);
  const std::vector<uint8_t> original = RestoreOriginalDol(dol);
  if (original.empty() || Sha1Hex(original) != kMainDolSha1)
    return "The game files don't match this version.";
  const std::vector<uint8_t> wanted = PatchDol(original, fps60, multiplier);
  if (wanted != dol && !WriteFile(dol_path, wanted))
    return "Could not update the game files.";
  return {};
}
}  // namespace disc_setup
