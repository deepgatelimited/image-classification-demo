#!/usr/bin/env bash
# Flash an app over the ST-LINK, unless the board already runs this build
# (recorded in firmware/build/last_flashed; delete it to force a flash).
# Usage: tools/flash.sh <app>   (capture | classify)
set -euo pipefail
cd "$(dirname "$0")/.."
app=${1:?usage: tools/flash.sh <app>}
elf=firmware/build/apps/$app/$app.elf
[ -f "$elf" ] || { echo "no $elf: run tools/build.sh $app first" >&2; exit 1; }

build="$app $(shasum -a 256 "$elf" | cut -d' ' -f1)"
if [ -f firmware/build/last_flashed ] && [ "$build" = "$(cat firmware/build/last_flashed)" ]; then
  echo "board already runs this $app build"
  exit 0
fi

rm -f firmware/build/last_flashed
if ! openocd -f board/stm32h747i-disco.cfg -c "program $elf verify reset exit"; then
  echo "flash failed: is the board's ST-LINK USB (CN2) plugged in?" >&2
  exit 1
fi
echo "$build" > firmware/build/last_flashed
sleep 1  # let the board boot and its USB serial port settle
