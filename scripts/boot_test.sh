#!/usr/bin/env bash
# boot_test.sh <seconds> <graphics> <outdir>: boot the built module, screenshot every 10 s, log status.
# Linux without a display: xvfb-run -a scripts/boot_test.sh 200 Vulkan out/
set -u
secs=$1; gfx=$2; out=$3
T="${TEMPLATE:?set TEMPLATE to your ModernGekko-Template checkout}"
M=$(realpath $T/build/modules/GC6E01/*/gGC6E01_recomp.so)
rm -rf "$out"; mkdir -p "$out/auto/commands" "$out/shots"
$T/lib/ModernGekko/build/moderngekko-run --game "$(realpath $T/extracted/Pokemon-Colosseum-USA)" \
  --module "$M" --user-dir "$out/user" --graphics "$gfx" --audio Null --mute \
  --automation-dir "$out/auto" > "$out/log.txt" 2>&1 &
pid=$!
for ((t=10; t<=secs; t+=10)); do
  sleep 10
  kill -0 $pid 2>/dev/null || break
  printf 'command=screenshot\npath=%s/shots/t%03d.png\n' "$out" $t > "$out/auto/commands/$(printf %05d $t).txt"
  { echo "--- t=$t"; grep -E "^(state|booted|fps|vps|speed|frame_count|present_count|last_error)=" "$out/auto/status.txt" 2>/dev/null; } >> "$out/status_log.txt"
done
kill $pid 2>/dev/null; wait $pid 2>/dev/null
echo "exit/killed"; tail -3 "$out/log.txt"
