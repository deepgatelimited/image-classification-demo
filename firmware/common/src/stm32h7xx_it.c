/* Cortex-M7 exception handlers. Peripheral IRQ handlers live with the app that uses them. */
#include "board.h"

void NMI_Handler(void) {}

/* Faults: red LED + halt, so a crash is visible without a debugger attached. */
void HardFault_Handler(void)  { Error_Handler(); }
void MemManage_Handler(void)  { Error_Handler(); }
void BusFault_Handler(void)   { Error_Handler(); }
void UsageFault_Handler(void) { Error_Handler(); }

void SVC_Handler(void) {}
void DebugMon_Handler(void) {}
void PendSV_Handler(void) {}

void SysTick_Handler(void) { HAL_IncTick(); }
