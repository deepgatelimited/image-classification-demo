#!/usr/bin/env bash
# Build `classify` for a model from the DeepGate platform and flash it.
#
# Usage: ./deploy.sh [model dir]   (default: the newest folder in tasks/helmet/models/)
#
# The model dir is tasks/<task>/models/<name>/ and holds what you downloaded:
#   *model_spec*.json            from the training job
#   deepgate-build-hard-*.zip    the build for the STM32H747XI, float ABI hard
set -euo pipefail
cd "$(dirname "$0")"
source env.sh

model=${1:-$(ls -td tasks/helmet/models/*/ 2>/dev/null | head -1 || true)}
[ -d "$model" ] || { echo "no model folder: pass tasks/<task>/models/<name>" >&2; exit 1; }
model=$(cd "$model" && pwd)
task=$(basename "$(dirname "$(dirname "$model")")")
[ -f "tasks/$task/labels.h" ] || { echo "no tasks/$task/labels.h: put the model in tasks/<task>/models/" >&2; exit 1; }
echo "task: $task, model: $(basename "$model")"

ls "$model"/*model_spec*.json >/dev/null 2>&1 || { echo "no *model_spec*.json in $model" >&2; exit 1; }
zip=$(ls "$model"/deepgate-build-hard-*.zip 2>/dev/null | head -1 || true)
[ -n "$zip" ] || { echo "no deepgate-build-hard-*.zip in $model" >&2; exit 1; }

# The compiled model (libmodel.a, model.h) and the header with its preprocessing.
rm -rf "$model/build"
unzip -q "$zip" -d "$model/build"
ls "$model"/build/*/lib/libmodel.a >/dev/null 2>&1 || { echo "$(basename "$zip") has no libmodel.a" >&2; exit 1; }
uv run --quiet --with numpy python tools/gen_model_meta.py "$model"

cmake -S firmware -B firmware/build -G Ninja -DTASK="$task" -DMODEL_DIR="$model"
tools/build.sh classify
tools/flash.sh classify
echo "done: ./mirror.sh shows the screen"
