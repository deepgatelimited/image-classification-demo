/* serial_link: binary frames to the laptop over the ST-LINK virtual COM port
 * (USART1), and one-byte commands back. Used by capture (camera frames) and
 * mirror (the screen).
 *
 * Board -> laptop: link_send() hands a buffer to DMA. printf is only safe while
 * !link_busy(), so text never lands inside a frame.
 * Laptop -> board: one byte at a time. Any byte is a heartbeat ('H', every 0.5 s);
 * 'G' and 'S' are also kept as commands. With no heartbeat for 1.5 s, the
 * laptop is gone and the app should stop sending. */
#ifndef SERIAL_LINK_H
#define SERIAL_LINK_H

#include <stdint.h>

/* USART1 at `baud` (printf goes there too), its TX DMA and RX interrupt. */
void link_init(uint32_t baud);

/* Starts sending buf[0..len). buf must be 32-byte aligned in AXI SRAM, with room
 * up to the next 32-byte boundary (the D-cache is cleaned in whole lines), and
 * stay untouched until !link_busy(). */
void link_send(const uint8_t *buf, uint32_t len);

/* A frame is still going out. */
int link_busy(void);

/* A heartbeat arrived in the last 1.5 s. */
int link_host_listening(void);

/* The last 'G' or 'S' received since the previous call, else 0. */
int link_take_command(void);

/* zlib's crc32, so the laptop checks it with zlib.crc32. */
uint32_t link_crc32(const uint8_t *p, uint32_t n);

static inline void link_put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void link_put_u32(uint8_t *p, uint32_t v) { link_put_u16(p, (uint16_t)v); link_put_u16(p + 2, (uint16_t)(v >> 16)); }

#endif
