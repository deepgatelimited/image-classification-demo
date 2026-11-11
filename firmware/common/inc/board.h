/* Board bring-up shared by every app: MPU, caches, HAL, 400 MHz clock. */
#ifndef BOARD_H
#define BOARD_H

#include "stm32h7xx_hal.h"
#include "stm32h747i_discovery.h"

/* Call first in main(). Leaves SysTick at 1 ms and the M7 at 400 MHz. */
void board_init(void);

/* Starts the ST-LINK virtual COM port (USART1) at `baud` 8N1; printf goes there.
 * The ST-LINK V3 bridge follows whatever rate the host opens the port at. */
void board_console_init(uint32_t baud);

/* Red LED on, spin forever. Used for unrecoverable errors. */
void Error_Handler(void);

#endif
