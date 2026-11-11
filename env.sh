# source env.sh: puts the Arm GNU toolchain on PATH (set ARM_GCC_ROOT to yours).
export ARM_GCC_ROOT="${ARM_GCC_ROOT:-$HOME/.local/toolchains/arm-gnu-toolchain-15.3.rel1-darwin-arm64-arm-none-eabi}"
export PATH="$ARM_GCC_ROOT/bin:$PATH"
