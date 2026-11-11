# classify: camera -> DeepGate-compiled classifier -> LCD. ./deploy.sh sets both:
# TASK names tasks/<TASK>/ (its labels.h styles the screen); MODEL_DIR holds the
# DeepGate build (libmodel.a and model.h, anywhere below it) and model_meta.h.
set(TASK "" CACHE STRING "task in tasks/ that classify is built for")
set(MODEL_DIR "" CACHE PATH "model directory for classify")
set(TASK_DIR "${CMAKE_SOURCE_DIR}/../tasks/${TASK}")

# Only glob inside a real folder: an empty MODEL_DIR would glob from / .
if(IS_DIRECTORY "${MODEL_DIR}")
  file(GLOB_RECURSE MODEL_LIB "${MODEL_DIR}/*libmodel.a")
  file(GLOB_RECURSE MODEL_HEADER "${MODEL_DIR}/*model.h")
  list(FILTER MODEL_HEADER EXCLUDE REGEX "model_meta\\.h$")
endif()
if(NOT TASK OR NOT EXISTS ${TASK_DIR}/labels.h OR NOT MODEL_DIR OR NOT MODEL_LIB OR NOT MODEL_HEADER
   OR NOT EXISTS ${MODEL_DIR}/model_meta.h)
  message(STATUS "classify skipped: set TASK and MODEL_DIR (./deploy.sh does it)")
  return()
endif()
list(GET MODEL_LIB 0 MODEL_LIB)
list(GET MODEL_HEADER 0 MODEL_HEADER)
get_filename_component(MODEL_INCLUDE ${MODEL_HEADER} DIRECTORY)
message(STATUS "classify: task ${TASK}, model ${MODEL_DIR}")

add_app(classify main.c)
target_include_directories(classify PRIVATE ${MODEL_INCLUDE} ${MODEL_DIR} ${TASK_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
# -u _printf_float: newlib-nano's printf leaves out %f unless asked; m: expf.
target_link_libraries(classify PRIVATE display cam serial_link mirror ${MODEL_LIB} m)
target_link_options(classify PRIVATE -u _printf_float)
