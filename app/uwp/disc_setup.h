// Disc checks, unpacking and main.dol patching for the Xbox app (the same steps as the Windows
// launcher and the Android app): the player's Pokemon Colosseum (USA) disc image (ISO, GCM or
// CISO) is checked to be GC6E01 with the supported main.dol and unpacked into Game\sys and
// Game\files; before each launch main.dol gets the patches the recompiled code was built with.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace disc_setup
{
// Reads `size` bytes at `offset` of the image file; false on a short read or error.
using ReadAt = std::function<bool(uint64_t offset, void* out, size_t size)>;

// Unpacks the image into `game`. Returns "" or a message for the player.
std::string SetupGame(const ReadAt& read, const std::filesystem::path& game,
                      std::atomic<int>& progress_percent);

bool GameReady(const std::filesystem::path& game);

// Patches Game\sys\main.dol to match the recompiled code. Returns "" or a message.
std::string PrepareDol(const std::filesystem::path& game, bool fps60, const std::string& aspect);
}  // namespace disc_setup
