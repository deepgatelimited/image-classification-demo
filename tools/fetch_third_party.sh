#!/usr/bin/env bash
# Fetch STM32CubeH7 (HAL, CMSIS, DISCO BSP, OV5640 driver, examples) into third_party/.
set -euo pipefail
cd "$(dirname "$0")/.."
if [ ! -d third_party/STM32CubeH7 ]; then
  git clone --depth 1 --recurse-submodules --shallow-submodules \
    https://github.com/STMicroelectronics/STM32CubeH7.git third_party/STM32CubeH7
fi
