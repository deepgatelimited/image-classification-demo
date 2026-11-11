/* capture: the data-collection firmware. Live camera on the LCD; the blue button
 * starts and stops a burst that streams frames to the laptop, where
 * tools/capture.py (./capture.sh) saves them as PNGs.
 *
 *   camera (320x240 RGB565) --> LCD preview + REC / count panel
 *                           '-> grab_frame(): copy into tx_buf = header | pixels | crc32
 *                                  --> serial_link (USART1 DMA) --> tools/capture.py
 *
 * Frames go out exactly as the camera delivered them (raw RGB565), so the PNGs
 * hold the pixels the classify firmware will see.
 *
 * Wire format of one frame, little-endian:
 *   "DGCF" | u32 seq | u16 width | u16 height | u8 format (1 = RGB565) | 3 zero bytes
 *   | width * height * 2 bytes of pixels, row-major from the top left
 *   | u32 crc32 of the pixels (zlib's crc32)
 * Status lines (printf) go out only between frames.
 *
 * The board records only while the laptop's heartbeats arrive, so it pauses when
 * the script exits or the cable comes out. 'G' / 'S' from the laptop start / stop
 * a burst like the button. */
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "cam.h"
#include "display.h"
#include "serial_link.h"
#include "stm32_lcd.h"

/* The link sets the burst rate: 153,616 bytes a frame is ~0.77 s at 2 Mbaud.
 * tools/capture_link.py opens the port at the same rate. */
#define CAPTURE_BAUD        2000000
/* Minimum time between two grabbed frames; raise it to spread a burst out. */
#define CAPTURE_INTERVAL_MS 500
#define BUTTON_DEBOUNCE_MS  250

#define FRAME_MAGIC  0x46434744u  /* "DGCF" as a little-endian u32 */
#define FRAME_RGB565 1
#define HEADER_BYTES 16
#define PIXEL_BYTES  (CAM_W * CAM_H * 2)
#define FRAME_BYTES  (HEADER_BYTES + PIXEL_BYTES + 4)

/* A copy of the frame being sent: the camera overwrites its buffer every ~85 ms,
 * and sending takes longer. Whole cache lines, for link_send(). */
static uint8_t tx_buf[(FRAME_BYTES + 31) & ~31u] __attribute__((section(".axisram"), aligned(32)));

static volatile uint32_t button_pressed;
static uint32_t button_last_ms;

void EXTI15_10_IRQHandler(void) { BSP_PB_IRQHandler(BUTTON_WAKEUP); }

/* The blue (wake-up) button, PC13. */
void BSP_PB_Callback(Button_TypeDef Button)
{
  uint32_t now = HAL_GetTick();
  if (Button != BUTTON_WAKEUP || now - button_last_ms < BUTTON_DEBOUNCE_MS) return;
  button_last_ms = now;
  button_pressed = 1;
}

/* Copies the frame into tx_buf with its header and crc and starts sending it.
 * The copy takes well under a millisecond, so it stays ahead of the camera DMA. */
static void grab_frame(const uint16_t *frame, uint32_t seq)
{
  uint8_t *pixels = tx_buf + HEADER_BYTES;
  memcpy(pixels, frame, PIXEL_BYTES);
  memset(tx_buf, 0, HEADER_BYTES);
  link_put_u32(tx_buf, FRAME_MAGIC);
  link_put_u32(tx_buf + 4, seq);
  link_put_u16(tx_buf + 8, CAM_W);
  link_put_u16(tx_buf + 10, CAM_H);
  tx_buf[12] = FRAME_RGB565;
  link_put_u32(pixels + PIXEL_BYTES, link_crc32(pixels, PIXEL_BYTES));
  link_send(tx_buf, FRAME_BYTES);
}

int main(void)
{
  board_init();
  link_init(CAPTURE_BAUD);
  printf("\r\ncapture: %dx%d RGB565 frames at %d baud\r\n", CAM_W, CAM_H, CAPTURE_BAUD);

  if (display_init() != 0) {
    printf("display init failed\r\n");
    Error_Handler();
  }
  UTIL_LCD_Clear(UTIL_LCD_COLOR_BLACK);
  if (cam_start() != 0) Error_Handler();
  if (BSP_PB_Init(BUTTON_WAKEUP, BUTTON_MODE_EXTI) != BSP_ERROR_NONE) Error_Handler();

  const uint32_t img_w = 2 * CAM_W, img_h = 2 * CAM_H;
  const uint32_t panel_x = img_w + 12;
  UTIL_LCD_SetBackColor(UTIL_LCD_COLOR_BLACK);
  UTIL_LCD_SetFont(&Font16);
  UTIL_LCD_SetTextColor(UTIL_LCD_COLOR_GRAY);
  UTIL_LCD_DisplayStringAt(panel_x, 16, (uint8_t *)"capture", LEFT_MODE);
  UTIL_LCD_DisplayStringAt(panel_x, 36, (uint8_t *)"320x240", LEFT_MODE);
  UTIL_LCD_SetFont(&Font12);
  UTIL_LCD_DisplayStringAt(panel_x, 440, (uint8_t *)"BLUE button:", LEFT_MODE);
  UTIL_LCD_DisplayStringAt(panel_x, 456, (uint8_t *)"start / stop", LEFT_MODE);
  display_show();

  uint32_t capturing = 0, seq = 0, sent = 0, sent_last = 0, flash = 0;
  uint32_t was_busy = 0, last_grab = 0, frames_last = 0, t_last = HAL_GetTick();
  while (1) {
    const uint16_t *frame = cam_wait_frame();

    int host = link_host_listening();
    uint32_t want = capturing;
    if (button_pressed) {
      button_pressed = 0;
      want = !capturing;
    }
    int command = link_take_command();
    if (command) want = command == 'G';
    if (!host) want = 0;  /* no laptop listening: stay paused */
    if (want != capturing && host && !link_busy()) printf("capture %s\r\n", want ? "started" : "stopped");
    capturing = want;

    if (was_busy && !link_busy()) sent++;
    if (capturing && !link_busy() && HAL_GetTick() - last_grab >= CAPTURE_INTERVAL_MS) {
      last_grab = HAL_GetTick();
      grab_frame(frame, seq++);
      flash = 2;  /* white border on the next two frames: "got one" */
    }
    was_busy = link_busy();

    display_wait();
    display_draw_rgb565_2x(frame, CAM_W, CAM_H, 0, 0);
    if (flash) {
      flash--;
      display_draw_border(0, 0, img_w, img_h, 10, UTIL_LCD_COLOR_WHITE);
    } else if (capturing) {
      display_draw_border(0, 0, img_w, img_h, 6, UTIL_LCD_COLOR_RED);
    }

    char line[24];
    UTIL_LCD_SetFont(&Font24);
    UTIL_LCD_SetTextColor(capturing ? UTIL_LCD_COLOR_RED : host ? UTIL_LCD_COLOR_GRAY : UTIL_LCD_COLOR_ORANGE);
    UTIL_LCD_DisplayStringAt(panel_x, 90, (uint8_t *)(capturing ? "REC    " : host ? "PAUSED " : "NO HOST"),
                             LEFT_MODE);
    UTIL_LCD_SetTextColor(UTIL_LCD_COLOR_WHITE);
    snprintf(line, sizeof line, "%-6lu", sent);
    UTIL_LCD_DisplayStringAt(panel_x, 130, (uint8_t *)line, LEFT_MODE);
    UTIL_LCD_SetFont(&Font16);
    UTIL_LCD_SetTextColor(UTIL_LCD_COLOR_GRAY);
    UTIL_LCD_DisplayStringAt(panel_x, 158, (uint8_t *)"images sent", LEFT_MODE);
    UTIL_LCD_SetFont(&Font12);
    UTIL_LCD_DisplayStringAt(panel_x, 400, (uint8_t *)(host ? "laptop: listening" : "run ./capture.sh "),
                             LEFT_MODE);
    UTIL_LCD_SetFont(&Font16);

    if (HAL_GetTick() - t_last >= 1000) {
      uint32_t frames = cam_frames();
      snprintf(line, sizeof line, "%3lu img/s ", sent - sent_last);
      UTIL_LCD_DisplayStringAt(panel_x, 200, (uint8_t *)line, LEFT_MODE);
      snprintf(line, sizeof line, "cam %2lu fps", frames - frames_last);
      UTIL_LCD_DisplayStringAt(panel_x, 220, (uint8_t *)line, LEFT_MODE);
      if (!link_busy())
        printf("status %s | sent %lu | camera %lu fps | errors %lu\r\n",
               capturing ? "REC" : "paused", sent, frames - frames_last, cam_errors());
      sent_last = sent;
      frames_last = frames;
      t_last += 1000;
      BSP_LED_Toggle(LED_GREEN);
    }
    display_show();
  }
}
