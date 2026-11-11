#!/usr/bin/env bash
# Flash `classify` if needed (./deploy.sh builds it), then show the board's
# screen at http://127.0.0.1:8766.
#
# Usage: ./mirror.sh [--record take.mp4] [--no-browser]
set -euo pipefail
cd "$(dirname "$0")"

tools/build.sh classify
tools/flash.sh classify
exec tools/mirror.py "$@"
