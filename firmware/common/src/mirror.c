/* mirror: the LCD's framebuffer -> tools/mirror.py over USART1 (ST-LINK VCP).
 * The wire format and usage are in mirror.h.
 *
 *   framebuffer (SDRAM, 800x480 ARGB8888, as last drawn)
 *     |-- left MIRROR_IMAGE_W columns: every 2nd pixel of every 2nd row -> Y, Cb, Cr
 *     '-- the rest: runs of palette colours (text, bars, flat backgrounds)
 *           |
 *           tx_buf[] (AXI SRAM) = header | payload | crc32
 *           |
 *           serial_link: USART1 TX DMA, in the background
 *
 * At 12.5 Mbaud the image planes alone (115 KB) take ~92 ms; classify draws a
 * frame every ~110 ms, so the two keep pace. */
#include "board.h"
#include "display.h"
#include "mirror.h"
#include "serial_link.h"

#define FRAME_MAGIC      0x464D4744u  /* "DGMF" as a little-endian u32 */
#define FORMAT_YCBCR420  1
#define FLAG_NO_PANEL    1u
#define HEADER_BYTES     32
#define IMG_W            (MIRROR_IMAGE_W / 2)
#define IMG_H            (DISPLAY_H / 2)
#define PANEL_X          MIRROR_IMAGE_W
#define PANEL_W          (DISPLAY_W - MIRROR_IMAGE_W)
#define Y_BYTES          (IMG_W * IMG_H)
#define C_BYTES          (Y_BYTES / 4)
#define MAX_COLOURS      255  /* palette_n is a u8 */
#define MAX_BYTES        (HEADER_BYTES + Y_BYTES + 2 * C_BYTES + MIRROR_RLE_MAX + MAX_COLOURS * 4 + 4)
#define TOO_BIG          0xFFFFFFFFu

/* Rounded up to whole 32-byte cache lines for link_send()'s cache clean. */
static uint8_t tx_buf[(MAX_BYTES + 31) & ~31u] __attribute__((section(".axisram"), aligned(32)));
static uint32_t palette[MAX_COLOURS];
static uint32_t seq;

void mirror_init(void) { link_init(MIRROR_BAUD); }
int mirror_idle(void) { return !link_busy(); }

static uint8_t clamp255(int v) { return v > 255 ? 255 : (uint8_t)v; }

/* JFIF full-range YCbCr in 8.8 fixed point; the +32768 keeps the shifts on
 * non-negative numbers. Cb and Cr come from the mean of each 2x2 block. */
static uint8_t *encode_image(const uint32_t *fb, uint8_t *out)
{
  uint8_t *y_plane = out, *cb = out + Y_BYTES, *cr = cb + C_BYTES;
  for (uint32_t by = 0; by < IMG_H / 2; by++) {
    const uint32_t *row0 = fb + 4 * by * DISPLAY_W;  /* image rows 2by, 2by+1 = screen rows 4by, 4by+2 */
    const uint32_t *row1 = row0 + 2 * DISPLAY_W;
    uint8_t *y0 = y_plane + 2 * by * IMG_W, *y1 = y0 + IMG_W;
    for (uint32_t bx = 0; bx < IMG_W / 2; bx++) {
      const uint32_t px[4] = {row0[4 * bx], row0[4 * bx + 2], row1[4 * bx], row1[4 * bx + 2]};
      uint8_t *ys[4] = {&y0[2 * bx], &y0[2 * bx + 1], &y1[2 * bx], &y1[2 * bx + 1]};
      int rs = 0, gs = 0, bs = 0;
      for (int k = 0; k < 4; k++) {
        int r = (px[k] >> 16) & 0xFF, g = (px[k] >> 8) & 0xFF, b = px[k] & 0xFF;
        *ys[k] = (uint8_t)((77 * r + 150 * g + 29 * b + 128) >> 8);
        rs += r, gs += g, bs += b;
      }
      int r = (rs + 2) >> 2, g = (gs + 2) >> 2, b = (bs + 2) >> 2;
      cb[by * (IMG_W / 2) + bx] = clamp255((-43 * r - 85 * g + 128 * b + 32768 + 128) >> 8);
      cr[by * (IMG_W / 2) + bx] = clamp255((128 * r - 107 * g - 21 * b + 32768 + 128) >> 8);
    }
  }
  return out + Y_BYTES + 2 * C_BYTES;
}

static int palette_index(uint32_t colour, uint32_t *n)
{
  for (uint32_t i = 0; i < *n; i++)
    if (palette[i] == colour) return (int)i;
  if (*n == MAX_COLOURS) return -1;
  palette[*n] = colour;
  return (int)(*n)++;
}

/* The panel as (palette index, run length) pairs in row-major order, runs carrying
 * on across rows. Returns the bytes written, or TOO_BIG. */
static uint32_t encode_panel(const uint32_t *fb, uint8_t *out, uint32_t *n_colours)
{
  uint32_t used = 0, run = 0, current = 0;
  int index = 0;
  *n_colours = 0;
  for (uint32_t y = 0; y < DISPLAY_H; y++) {
    const uint32_t *row = fb + y * DISPLAY_W + PANEL_X;
    for (uint32_t x = 0; x < PANEL_W; x++) {
      if (run && row[x] == current && run < 255) {
        run++;
        continue;
      }
      if (run) {
        if (used + 2 > MIRROR_RLE_MAX) return TOO_BIG;
        out[used++] = (uint8_t)index;
        out[used++] = (uint8_t)run;
      }
      current = row[x];
      if ((index = palette_index(current, n_colours)) < 0) return TOO_BIG;
      run = 1;
    }
  }
  if (used + 2 > MIRROR_RLE_MAX) return TOO_BIG;
  out[used++] = (uint8_t)index;
  out[used++] = (uint8_t)run;
  return used;
}

void mirror_send(void)
{
  if (link_busy() || !link_host_listening()) return;

  const uint32_t *fb = display_framebuffer();
  /* DMA2D draws into the SDRAM behind the D-cache. The SDRAM is write-through, so
   * no line is dirty: dropping the cached copy loses nothing. */
  SCB_InvalidateDCache_by_Addr((void *)fb, DISPLAY_W * DISPLAY_H * 4);

  uint8_t *payload = tx_buf + HEADER_BYTES;
  uint8_t *p = encode_image(fb, payload);
  uint32_t n_colours, flags = 0;
  uint32_t rle_bytes = encode_panel(fb, p, &n_colours);
  if (rle_bytes == TOO_BIG) {
    rle_bytes = n_colours = 0;
    flags |= FLAG_NO_PANEL;
  }
  p += rle_bytes;
  for (uint32_t i = 0; i < n_colours; i++, p += 4) link_put_u32(p, palette[i]);
  uint32_t payload_bytes = (uint32_t)(p - payload);
  link_put_u32(p, link_crc32(payload, payload_bytes));
  p += 4;

  link_put_u32(tx_buf, FRAME_MAGIC);
  link_put_u32(tx_buf + 4, seq);
  link_put_u16(tx_buf + 8, DISPLAY_W);
  link_put_u16(tx_buf + 10, DISPLAY_H);
  link_put_u16(tx_buf + 12, MIRROR_IMAGE_W);
  tx_buf[14] = FORMAT_YCBCR420;
  tx_buf[15] = (uint8_t)n_colours;
  link_put_u32(tx_buf + 16, rle_bytes);
  link_put_u32(tx_buf + 20, payload_bytes);
  link_put_u32(tx_buf + 24, flags);
  link_put_u32(tx_buf + 28, HAL_GetTick());

  seq++;
  link_send(tx_buf, (uint32_t)(p - tx_buf));
}
