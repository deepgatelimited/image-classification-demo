#!/usr/bin/env bash
# Flash `capture` if needed, then save the board's frames as PNGs in
# tasks/<task>/data/raw/session-N/. Blue button: start / stop. Ctrl-C: done.
#
# Usage: ./capture.sh [task] [tools/capture.py options, e.g. --max 200]
set -euo pipefail
cd "$(dirname "$0")"

task=helmet
if [ -n "${1:-}" ] && [ "${1#-}" = "$1" ]; then task=$1; shift; fi
tools/build.sh capture
tools/flash.sh capture

root=tasks/$task/data/raw
mkdir -p "$root"
n=1
while [ -e "$root/session-$n" ]; do n=$((n + 1)); done
exec tools/capture.py --out "$root/session-$n" "$@"
