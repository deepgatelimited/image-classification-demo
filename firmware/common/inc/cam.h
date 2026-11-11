/* cam: the OV5640 streaming 320x240 RGB565 into one buffer in AXI SRAM, by
 * DCMI + DMA, continuously. */
#ifndef VL_CAM_H
#define VL_CAM_H

#include <stdint.h>

#define CAM_W 320
#define CAM_H 240

/* Starts the camera. Returns 0, or prints why and returns nonzero. */
int cam_start(void);

/* Waits for the next frame and returns it, D-cache invalidated. Pixel (x, y) is
 * frame[y * CAM_W + x]. The DMA keeps overwriting it from the top, so read
 * (or copy) what you need right away. */
const uint16_t *cam_wait_frame(void);

/* Frames delivered and DCMI/DMA errors, since start. */
uint32_t cam_frames(void);
uint32_t cam_errors(void);

#endif
