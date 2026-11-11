/* mirror: a copy of the LCD, streamed to the laptop over the ST-LINK serial link,
 * where tools/mirror.py shows it in a browser (to screen-record with the rest of a
 * demo). It reads the finished framebuffer, so the laptop sees what the panel shows.
 *
 * Usage:
 *   mirror_init();
 *   once per frame, after drawing (before the next display_wait()):
 *     if (mirror_idle()) printf(...);   // text only between frames, never into one
 *     mirror_send();
 *
 * It sends only while the laptop's heartbeats (any byte, every 0.5 s) arrive, so
 * with nothing listening the console carries text alone. A frame takes about as
 * long to send as classify takes to draw one, so a frame that comes while the
 * last is still going is skipped.
 *
 * Wire format of one frame, little-endian:
 *   "DGMF" | u32 seq | u16 screen_w | u16 screen_h | u16 image_w | u8 format (1)
 *   | u8 palette_n | u32 rle_bytes | u32 payload_bytes | u32 flags | u32 tick_ms
 *   | payload: Y, Cb, Cr planes | rle_bytes of (u8 palette index, u8 run length)
 *              | palette_n x u32 ARGB
 *   | u32 crc32 of the payload (zlib's crc32)
 * The screen splits at image_w. Left of it (the 2x camera image and its border):
 * every second pixel of every second row as YCbCr 4:2:0 (JFIF full range, planes
 * of (image_w/2) x (screen_h/2), then a quarter of that each for Cb and Cr).
 * Right of it (the text panel): lossless, as runs of palette colours in row-major
 * order. Flag bit 0: the panel needed more than MIRROR_RLE_MAX bytes and was left
 * out; the laptop keeps the last one. */
#ifndef VL_MIRROR_H
#define VL_MIRROR_H

#include <stdint.h>

/* The fastest rate both ends agree on: USART1 at 100 MHz with 8x oversampling,
 * and the ST-LINK V3 bridge (8 Mbaud fails; 10 and 12.5 work). tools/mirror.py
 * opens the port at the same rate. */
#define MIRROR_BAUD    12500000
/* Left part of the screen sent at half resolution: the camera image drawn 2x. */
#define MIRROR_IMAGE_W 640
/* Room for the text panel's runs; a typical classify panel takes a few KB. */
#define MIRROR_RLE_MAX (48 * 1024)

/* Starts the console at MIRROR_BAUD (printf goes there) and the serial link. */
void mirror_init(void);

/* Nothing in flight: printf is safe. */
int mirror_idle(void);

/* If the laptop is listening and the link is idle, snapshot the framebuffer and
 * start sending it; otherwise return at once. A snapshot takes a few ms of CPU. */
void mirror_send(void);

#endif
