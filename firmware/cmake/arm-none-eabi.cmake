# CMake toolchain file for the Arm GNU toolchain (Cortex-M7, hard float).
# The toolchain root comes from ARM_GCC_ROOT (set by env.sh).
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(ARM_GCC_ROOT $ENV{ARM_GCC_ROOT})
if(NOT EXISTS "${ARM_GCC_ROOT}/bin/arm-none-eabi-gcc")
  message(FATAL_ERROR "No arm-none-eabi-gcc in ARM_GCC_ROOT='${ARM_GCC_ROOT}'. "
                      "Run 'source env.sh' (and set the toolchain path in it).")
endif()

set(CMAKE_C_COMPILER   ${ARM_GCC_ROOT}/bin/arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER ${ARM_GCC_ROOT}/bin/arm-none-eabi-g++)
set(CMAKE_ASM_COMPILER ${ARM_GCC_ROOT}/bin/arm-none-eabi-gcc)
set(CMAKE_OBJCOPY      ${ARM_GCC_ROOT}/bin/arm-none-eabi-objcopy)
set(CMAKE_SIZE         ${ARM_GCC_ROOT}/bin/arm-none-eabi-size)

# Without this, CMake tries to link a host-style test program and fails.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Must match the float ABI of any prebuilt library we link (DeepGate builds: hard).
set(MCU_FLAGS "-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard")
set(CMAKE_C_FLAGS_INIT   "${MCU_FLAGS} -ffunction-sections -fdata-sections -Wall")
set(CMAKE_CXX_FLAGS_INIT "${MCU_FLAGS} -ffunction-sections -fdata-sections -Wall -fno-exceptions -fno-rtti")
set(CMAKE_ASM_FLAGS_INIT "${MCU_FLAGS} -x assembler-with-cpp")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${MCU_FLAGS} --specs=nano.specs -Wl,--gc-sections -Wl,--print-memory-usage")
