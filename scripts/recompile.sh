#!/usr/bin/env bash
# Recompile Colosseum's main.dol to C with DolRecomp.
#   scripts/recompile.sh <path/to/game.(iso|ciso)>
# Needs DOLRECOMP pointing at a built dolrecomp binary.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
src="$1"; DOLRECOMP="${DOLRECOMP:-dolrecomp}"
iso="$src"
if [[ "$src" == *.ciso ]]; then iso="$here/iso/colosseum.iso"; mkdir -p "$here/iso"
  python3 -I "$here/tools/disc.py" ciso2iso "$src" "$iso"; fi
python3 -I "$here/tools/disc.py" verify "$iso"
python3 -I "$here/tools/disc.py" extract "$iso" "$here/extracted"
python3 -I "$here/tools/disc.py" patch-dol "$here/extracted/sys/main.dol" "$here/extracted/patched-main.dol"
"$DOLRECOMP" --gamecube "$here/extracted/patched-main.dol" "$here/recomp"
# The module build embeds tables from the exact DOL that was recompiled.
cp "$here/extracted/patched-main.dol" "$here/recomp/generated/main.dol"
