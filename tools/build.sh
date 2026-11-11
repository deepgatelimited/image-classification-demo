#!/usr/bin/env bash
# Build an app (configuring firmware/build the first time).
# Usage: tools/build.sh <app>   (capture | classify)
set -euo pipefail
cd "$(dirname "$0")/.."
source env.sh
app=${1:?usage: tools/build.sh <app>}

[ -f firmware/build/build.ninja ] || cmake -S firmware -B firmware/build -G Ninja
if ! cmake --build firmware/build --target "$app"; then
  [ "$app" != classify ] || echo "no classify build: run ./deploy.sh first" >&2
  exit 1
fi
