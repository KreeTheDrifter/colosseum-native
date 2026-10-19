#!/usr/bin/env bash
# Builds the Xbox (UWP, Developer Mode) package on Linux, using the same checkout, patches and
# recompiled code as scripts/build_windows.sh (run that first; it leaves the work tree in
# $WORK, default ./build-win).
#
#   scripts/build_xbox.sh [version]   ->  out-xbox/ColosseumNative-Xbox.msix + ColosseumNative.cer
#
# Toolchain: llvm-mingw's x86_64-w64-mingw32uwp target. It links against windowsapp.lib and
# ucrtapp only, so any API the Xbox doesn't allow to UWP apps fails at link time. Packing uses
# makemsix (github.com/microsoft/msix-packaging, built with --pack); signing uses osslsigncode.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
work="${WORK:-$here/build-win}"
out="$here/out-xbox"
version="${1:-1.0.0.0}"
jobs="${JOBS:-$(nproc)}"
export LLVM_MINGW="$work/llvm-mingw"
export CASE_SHIM="$work/case-shim"
export PKG_CONFIG_LIBDIR=/nonexistent
T="$work/template"
MG="$T/lib/ModernGekko"
TC="$here/cmake/llvm-mingw-uwp-x86_64.cmake"
# The package signing key stays outside the repository.
signing="${SIGNING_DIR:-$HOME/.colosseum-native/xbox-signing}"

cp "$here/runtime/game_menu.cpp" "$here/runtime/game_menu.hpp" "$MG/src/frontend/"

# 1. Runtime + app (ColosseumXbox.exe).
cmake -S "$MG" -B "$work/runtime-uwp" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$TC" \
  -DBUILD_TESTING=OFF -DDOLRECOMP_ENABLE_LLVM=OFF -DMODERNGEKKO_UWP=ON \
  -DMODERNGEKKO_UWP_APP_DIR="$here/app/uwp"
cmake --build "$work/runtime-uwp" -j"$jobs" --target ColosseumXbox

# 2. Game module against the UWP runtime libraries (same flags as the Windows build).
cmake -S "$MG/vendor/dolphin/module-template" -B "$work/module-uwp" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$TC" -DGAME_ID=GC6E01 -DGENERATED_DIR="$here/recomp/generated" \
  -DGXRUNTIME_DIR="$MG/vendor/dolphin/GXRuntime" \
  -DCHASSIS_ABI_DIR="$MG/vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp" \
  -DCMAKE_C_FLAGS="-march=x86-64-v2 -DGXRUNTIME_GAMECUBE_ONLY -DGXRUNTIME_RAM_SIZE=0x01800000u -DGXRUNTIME_NO_WRITE_JOURNAL" \
  -DRECOMPCORE_MODULE_ENABLE_IPO=ON -DRECOMPCORE_MODULE_OPT_LEVEL=3 -DPython3_EXECUTABLE="$(command -v python3)"
cmake --build "$work/module-uwp" -j"$jobs"

# 3. Check that every import is one UWP apps may use, then pack and sign.
python3 "$here/scripts/uwp_import_check.py" "$LLVM_MINGW" \
  "$work/runtime-uwp/uwp-app/ColosseumXbox.exe" "$work/module-uwp/gGC6E01_recomp.dll"
mkdir -p "$out"
python3 "$here/scripts/xbox_package.py" "$work/runtime-uwp/uwp-app/ColosseumXbox.exe" \
  "$work/module-uwp/gGC6E01_recomp.dll" "$MG/vendor/dolphin/Data/Sys" "$signing" \
  "$out/ColosseumNative-Xbox.msix" "$version"
cp "$signing/ColosseumNative.cer" "$out/"
echo "built: $out"
