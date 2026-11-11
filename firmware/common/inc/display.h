/* display: the DISCO's 800x480 DSI panel in adapted command mode, without tearing.
 *
 * Usage, once per frame:
 *   display_wait();   // previous frame fully handed to the panel: safe to draw
 *   ... draw into the framebuffer (UTIL_LCD_* or direct writes) ...
 *   display_show();   // send it to the panel at the next tearing-effect pulse
 *
 * The panel only changes when display_show() is called, so a half-drawn
 * framebuffer is never visible. */
#ifndef VL_DISPLAY_H
#define VL_DISPLAY_H

#include <stdint.h>

#define DISPLAY_W 800
#define DISPLAY_H 480

/* SDRAM, DSI host (command mode), LTDC, panel controller (NT35510 or OTM8009A,
 * probed by ID), TE pin, and ST's UTIL_LCD drawing layer. Returns 0 on success. */
int display_init(void);

/* The ARGB8888 framebuffer, DISPLAY_W x DISPLAY_H, row-major. */
uint32_t *display_framebuffer(void);

/* Queues a refresh: the next TE pulse starts sending the framebuffer. */
void display_show(void);

/* Blocks until the queued refresh has finished. */
void display_wait(void);

/* An RGB565 image (e.g. a camera frame), w x h, drawn at 2x with its top left at (x, y). */
void display_draw_rgb565_2x(const uint16_t *src, uint32_t w, uint32_t h, uint32_t x, uint32_t y);

/* A rectangle outline t pixels thick, ARGB8888 colour. */
void display_draw_border(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t t, uint32_t colour);

#endif
