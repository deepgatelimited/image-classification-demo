/* cam: see cam.h. Defines the DCMI and DMA2 stream 3 handlers and the BSP's
 * camera callbacks, so it is an OBJECT library (CMakeLists.txt). */
#include <stdio.h>
#include "cam.h"
#include "board.h"
#include "stm32h747i_discovery_camera.h"

/* DMA buffer: AXI SRAM (the DMA can't reach DTCM), whole cache lines. */
static uint16_t frame[CAM_W * CAM_H] __attribute__((section(".axisram"), aligned(32)));

static volatile uint32_t frames, errors, ready;

void DCMI_IRQHandler(void)         { BSP_CAMERA_IRQHandler(0); }
void DMA2_Stream3_IRQHandler(void) { BSP_CAMERA_DMA_IRQHandler(0); }

void BSP_CAMERA_FrameEventCallback(uint32_t Instance)
{
  (void)Instance;
  frames++;
  ready = 1;
}

void BSP_CAMERA_ErrorCallback(uint32_t Instance)
{
  (void)Instance;
  errors++;
}

int cam_start(void)
{
  int32_t err = BSP_CAMERA_Init(0, CAMERA_R320x240, CAMERA_PF_RGB565);
  if (err != BSP_ERROR_NONE) {
    printf("camera init failed: %ld (is the camera module seated?)\r\n", err);
    return 1;
  }
  if (BSP_CAMERA_Start(0, (uint8_t *)frame, CAMERA_MODE_CONTINUOUS) != BSP_ERROR_NONE) {
    printf("camera start failed\r\n");
    return 1;
  }
  return 0;
}

const uint16_t *cam_wait_frame(void)
{
  while (!ready) {}
  ready = 0;
  /* The DMA wrote behind the D-cache: drop stale lines before the CPU reads. */
  SCB_InvalidateDCache_by_Addr((void *)frame, sizeof frame);
  return frame;
}

uint32_t cam_frames(void) { return frames; }
uint32_t cam_errors(void) { return errors; }
