cmake_minimum_required(VERSION 3.22)
set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/cmake/arm-none-eabi.cmake)
project(image_class_demo_firmware C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()
set(CMAKE_C_FLAGS_DEBUG "-Og -g3")

set(STM32CUBEH7_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/STM32CubeH7
    CACHE PATH "STM32CubeH7 checkout (HAL, BSP, CMSIS)")
set(CUBE ${STM32CUBEH7_DIR})
if(NOT EXISTS ${CUBE}/Drivers)
  message(FATAL_ERROR "STM32CubeH7 missing at ${CUBE}: run tools/fetch_third_party.sh")
endif()

# --- board -------------------------------------------------------------------
# board_config: include paths + the defines ST's CubeIDE projects for this board use.
# USE_PWR_DIRECT_SMPS_SUPPLY is load-bearing: the DISCO powers the core from the
# on-chip SMPS; a wrong supply config can lock the chip until a recovery flash.
add_library(board_config INTERFACE)
target_include_directories(board_config INTERFACE
  common/inc
  ${CUBE}/Drivers/CMSIS/Include
  ${CUBE}/Drivers/CMSIS/Device/ST/STM32H7xx/Include
  ${CUBE}/Drivers/STM32H7xx_HAL_Driver/Inc
  ${CUBE}/Drivers/BSP/STM32H747I-DISCO
  ${CUBE}/Drivers/BSP/Components/Common
  ${CUBE}/Utilities/lcd
)
target_compile_definitions(board_config INTERFACE
  STM32H747xx CORE_CM7 USE_HAL_DRIVER USE_STM32H747I_DISCO USE_PWR_DIRECT_SMPS_SUPPLY
)

# st_drivers: ST's HAL + DISCO BSP as an archive. HAL sources are #ifdef-guarded by
# common/inc/stm32h7xx_hal_conf.h, so compiling all of them is cheap; the linker
# pulls only what an app calls. ST code is not warning-clean: -w there, -Wall in ours.
file(GLOB HAL_SRC ${CUBE}/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal*.c)
list(FILTER HAL_SRC EXCLUDE REGEX "_template\\.c$")
# HAL_SDRAM sits on the FMC low-layer driver, which the hal* glob misses.
list(APPEND HAL_SRC ${CUBE}/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_ll_fmc.c)
# Display: LCD BSP probes both panel controllers the DISCO shipped with (NT35510, then
# OTM8009A) and the HDMI bridge (ADV7533); its framebuffer lives in SDRAM (IS42S32800J).
# stm32_lcd.c is ST's text/shape drawing layer; it #includes the font tables itself.
set(BSP_SRC
  ${CUBE}/Drivers/BSP/STM32H747I-DISCO/stm32h747i_discovery.c
  ${CUBE}/Drivers/BSP/STM32H747I-DISCO/stm32h747i_discovery_bus.c
  ${CUBE}/Drivers/BSP/STM32H747I-DISCO/stm32h747i_discovery_lcd.c
  ${CUBE}/Drivers/BSP/STM32H747I-DISCO/stm32h747i_discovery_sdram.c
  ${CUBE}/Drivers/BSP/Components/nt35510/nt35510.c
  ${CUBE}/Drivers/BSP/Components/nt35510/nt35510_reg.c
  ${CUBE}/Drivers/BSP/Components/otm8009a/otm8009a.c
  ${CUBE}/Drivers/BSP/Components/otm8009a/otm8009a_reg.c
  ${CUBE}/Drivers/BSP/Components/adv7533/adv7533.c
  ${CUBE}/Drivers/BSP/Components/adv7533/adv7533_reg.c
  ${CUBE}/Drivers/BSP/Components/is42s32800j/is42s32800j.c
  ${CUBE}/Utilities/lcd/stm32_lcd.c
  # Camera: DCMI + DMA capture. The BSP probes OV9655 first, then our OV5640, over I2C4.
  ${CUBE}/Drivers/BSP/STM32H747I-DISCO/stm32h747i_discovery_camera.c
  ${CUBE}/Drivers/BSP/Components/ov5640/ov5640.c
  ${CUBE}/Drivers/BSP/Components/ov5640/ov5640_reg.c
  ${CUBE}/Drivers/BSP/Components/ov9655/ov9655.c
  ${CUBE}/Drivers/BSP/Components/ov9655/ov9655_reg.c
)
add_library(st_drivers STATIC ${HAL_SRC} ${BSP_SRC})
target_link_libraries(st_drivers PUBLIC board_config)
target_compile_options(st_drivers PRIVATE -w)

# board: our startup, syscalls and IRQ handlers, linked as objects, not from an
# archive: the linker only pulls archive members that resolve an already-undefined
# symbol, so strong IRQ handlers would lose to the startup file's weak defaults and
# newlib's _write/_sbrk would go unresolved.
add_library(board OBJECT
  common/src/startup_stm32h747xihx.s
  common/src/system_stm32h7xx.c
  common/src/board.c
  common/src/stm32h7xx_it.c
  common/src/syscalls.c
  common/src/sysmem.c
)
target_link_libraries(board PUBLIC board_config st_drivers)

# Shared drivers. OBJECT libraries like `board`, because each defines IRQ handlers
# or HAL callbacks: display (DSI panel, tear-free), cam (OV5640 over DCMI),
# serial_link (USART1 frames + heartbeats) and mirror (the screen over serial_link).
add_library(display OBJECT common/src/display.c)
add_library(cam OBJECT common/src/cam.c)
add_library(serial_link OBJECT common/src/serial_link.c)
add_library(mirror OBJECT common/src/mirror.c)
foreach(lib display cam serial_link mirror)
  target_link_libraries(${lib} PUBLIC board_config st_drivers)
endforeach()
target_link_libraries(mirror PUBLIC serial_link)  # usage only: apps link serial_link too
# The mirror's encoder runs once per frame: -O2 even in Debug builds.
target_compile_options(mirror PRIVATE -O2)

set(LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/common/STM32H747XIHX_FLASH.ld)

# add_app(<name> <sources...>) -> <name>.elf + <name>.bin
function(add_app name)
  add_executable(${name} ${ARGN})
  set_target_properties(${name} PROPERTIES SUFFIX ".elf")
  target_link_libraries(${name} PRIVATE board)
  target_link_options(${name} PRIVATE -T${LINKER_SCRIPT} -Wl,-Map=${name}.map)
  add_custom_command(TARGET ${name} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${name}> ${name}.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${name}>)
endfunction()

add_subdirectory(apps/capture)
add_subdirectory(apps/classify)
