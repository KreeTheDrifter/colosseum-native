#!/usr/bin/env bash
# Build the Windows x86-64 package of Colosseum Native from Linux.
#
#   scripts/build_windows.sh <disc image (.iso/.ciso)> <out dir>
#
# Output (private: it contains recompiled game code, never publish it):
#   ColosseumNative.exe   launcher (asks for the disc on first run, unpacks it)
#   moderngekko-run.exe   ModernGekko runtime
#   gGC6E01_recomp.dll    recompiled Pokemon Colosseum (USA) code
#   Sys\                  Dolphin data (GC6E01 game settings, GameCube fonts)
#   UpscaleTextures.exe   builds an AI-upscaled HD texture pack from dumped textures
#
# Needs: cmake, ninja, python3, git. Downloads llvm-mingw and ModernGekko-Template.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
disc="$(realpath "$1")"; out="$(realpath -m "$2")"
work="${WORK:-$here/build-win-work}"; mkdir -p "$work" "$out"
jobs="${JOBS:-$(nproc)}"

# 1. Toolchain: llvm-mingw (recent Windows headers + libc++ with C++23 bits).
export LLVM_MINGW="$work/llvm-mingw"
if [ ! -x "$LLVM_MINGW/bin/x86_64-w64-mingw32-clang" ]; then
  curl -sSL https://github.com/mstorsjo/llvm-mingw/releases/download/20250114/llvm-mingw-20250114-ucrt-ubuntu-20.04-x86_64.tar.xz \
    | tar xJ -C "$work"
  mv "$work"/llvm-mingw-20250114-ucrt-ubuntu-20.04-x86_64 "$LLVM_MINGW"
fi

# 2. ModernGekko + DolRecomp. bzip2's upstream is on GitLab; use the GitHub mirror.
T="$work/template"
if [ ! -d "$T/lib/ModernGekko/vendor/dolphin/Source" ]; then
  git config --global url."https://github.com/libarchive/bzip2.git".insteadOf "https://gitlab.com/bzip2/bzip2.git"
  git clone --recursive --depth 1 --shallow-submodules https://github.com/ExpansionPak/ModernGekko-Template.git "$T"
  # MinGW build fixes, in-game menu hooks (overlay, input routing, input gate).
  (cd "$T/lib/ModernGekko" && git apply "$here/patches/moderngekko.patch")
  (cd "$T/lib/ModernGekko/vendor/dolphin" && git apply "$here/patches/dolphin.patch")
fi
MG="$T/lib/ModernGekko"
mkdir -p "$MG/src/frontend"
cp "$here/runtime/game_menu.cpp" "$here/runtime/game_menu.hpp" "$MG/src/frontend/"

# 3. Case aliases: the code includes <Windows.h> etc.; MinGW ships lowercase names.
export CASE_SHIM="$work/case-shim"; rm -rf "$CASE_SHIM"; mkdir -p "$CASE_SHIM"
inc="$LLVM_MINGW/generic-w64-mingw32/include"
grep -rhoE '#\s*include\s*[<"][A-Za-z0-9_/]*[A-Z][A-Za-z0-9_/]*\.h[>"]' "$MG/src" "$MG/tools" "$MG/include" "$MG/vendor" \
  --include=*.c --include=*.cpp --include=*.h --include=*.hpp --include=*.inl 2>/dev/null \
  | sed -E 's/.*[<"](.*)[>"]/\1/' | sort -u | while read -r h; do
    lc=$(echo "$h" | tr A-Z a-z)
    if [ ! -e "$inc/$h" ] && [ -e "$inc/$lc" ]; then mkdir -p "$CASE_SHIM/$(dirname "$h")"; ln -sf "$inc/$lc" "$CASE_SHIM/$h"; fi
  done

TC="$here/cmake/llvm-mingw-x86_64.cmake"
export PKG_CONFIG_LIBDIR=/nonexistent   # never pick up the build machine's Linux libraries

# 4. Runtime (moderngekko-run.exe). Host tools (dolrecomp) are built natively for step 5.
cmake -S "$MG" -B "$work/runtime-win" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$TC" \
  -DBUILD_TESTING=OFF -DDOLRECOMP_ENABLE_LLVM=OFF
cmake --build "$work/runtime-win" -j"$jobs" --target moderngekko-run
cmake -S "$T/lib/DolRecomp" -B "$work/dolrecomp-host" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$work/dolrecomp-host" -j"$jobs"

# 5. Recompile main.dol and build the game module DLL.
DOLRECOMP="$work/dolrecomp-host/dolrecomp" "$here/scripts/recompile.sh" "$disc"
cmake -S "$MG/vendor/dolphin/module-template" -B "$work/module-win" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$TC" -DGAME_ID=GC6E01 -DGENERATED_DIR="$here/recomp/generated" \
  -DGXRUNTIME_DIR="$MG/vendor/dolphin/GXRuntime" \
  -DCHASSIS_ABI_DIR="$MG/vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp" \
  -DCMAKE_C_FLAGS="-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A00000C -idirafter $CASE_SHIM -march=x86-64-v2" \
  -DRECOMPCORE_MODULE_ENABLE_IPO=ON -DRECOMPCORE_MODULE_OPT_LEVEL=3 -DPython3_EXECUTABLE="$(command -v python3)"
# -O3 + ThinLTO + x86-64-v2 (SSE4.2/POPCNT; every Windows 11 PC) is about 10% faster than
# -O2 on the game code. FP contraction stays off (module template), so results don't change.
cmake --build "$work/module-win" -j"$jobs"

# 6. Launcher.
# The launcher refuses a game-code DLL from another build (it patches main.dol to match).
module_sha1=$(sha1sum "$work/module-win/gGC6E01_recomp.dll" | cut -d' ' -f1)
"$LLVM_MINGW/bin/x86_64-w64-mingw32-clang++" -std=c++20 -O2 -Wall -municode -mwindows -static \
  -D_WIN32_WINNT=0x0A00 -DCOLO_MODULE_SHA1="\"$module_sha1\"" "$here/app/launcher/colosseum_launcher.cpp" -lbcrypt -lcomdlg32 -ldxgi \
  -o "$out/ColosseumNative.exe"

# 7. HD texture builder (Real-ESRGAN is downloaded by the tool on first use).
"$LLVM_MINGW/bin/x86_64-w64-mingw32-clang++" -std=c++20 -O2 -static -D_WIN32_WINNT=0x0A00 \
  "$here/app/textools/upscale_textures.cpp" -lurlmon -o "$out/UpscaleTextures.exe"

cp "$work/runtime-win/moderngekko-run.exe" "$out/"
rm -rf "$out/Sys"; cp -r "$MG/vendor/dolphin/Data/Sys" "$out/Sys"   # game settings, GC fonts, DSP ROMs
cp "$here/app/shaders/"*.glsl "$out/Sys/Shaders/"                     # FidelityFX CAS / FSR 1
cp "$work/module-win/gGC6E01_recomp.dll" "$out/"
echo "built: $out"
