/* Board bring-up for the STM32H747I-DISCO, Cortex-M7 only.
 * Clock tree and MPU setup follow ST's STM32H747I-DISCO examples (STM32CubeH7;
 * the SDRAM region is from LCD_DSI_VideoMode_SingleBuffer). */
#include <string.h>
#include "board.h"

#if !defined(USE_PWR_DIRECT_SMPS_SUPPLY)
#error "STM32H747I-DISCO must be built with USE_PWR_DIRECT_SMPS_SUPPLY (see firmware/CMakeLists.txt)"
#endif

static void mpu_config(void)
{
  MPU_Region_InitTypeDef r = {0};

  HAL_MPU_Disable();
  /* Region 0: background no-access for undefined address space, so stray
   * pointers fault instead of silently reading garbage. */
  r.Enable = MPU_REGION_ENABLE;
  r.BaseAddress = 0x00;
  r.Size = MPU_REGION_SIZE_4GB;
  r.AccessPermission = MPU_REGION_NO_ACCESS;
  r.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
  r.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  r.IsShareable = MPU_ACCESS_SHAREABLE;
  r.Number = MPU_REGION_NUMBER0;
  r.TypeExtField = MPU_TEX_LEVEL0;
  r.SubRegionDisable = 0x87;
  r.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  HAL_MPU_ConfigRegion(&r);

  /* Region 1: the 32 MB SDRAM (FMC bank, 0xD0000000), which region 0 blocks.
   * Write-through: every CPU write goes straight to SDRAM, so bus masters that
   * read it behind the D-cache (LTDC scanning the framebuffer) see it without
   * cache maintenance. Reads are still cached. */
  r.Enable = MPU_REGION_ENABLE;
  r.BaseAddress = 0xD0000000;
  r.Size = MPU_REGION_SIZE_32MB;
  r.AccessPermission = MPU_REGION_FULL_ACCESS;
  r.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
  r.IsCacheable = MPU_ACCESS_CACHEABLE;
  r.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
  r.Number = MPU_REGION_NUMBER1;
  r.TypeExtField = MPU_TEX_LEVEL0;
  r.SubRegionDisable = 0x00;
  r.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;
  HAL_MPU_ConfigRegion(&r);

  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

/* HSE 25 MHz -> PLL1 (M=5, N=160, P=2) -> SYSCLK 400 MHz, HCLK 200 MHz. */
static void clock_config(void)
{
  RCC_OscInitTypeDef osc = {0};
  RCC_ClkInitTypeDef clk = {0};

  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
  while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  osc.HSEState = RCC_HSE_ON;
  osc.HSIState = RCC_HSI_OFF;
  osc.CSIState = RCC_CSI_OFF;
  osc.PLL.PLLState = RCC_PLL_ON;
  osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  osc.PLL.PLLM = 5;
  osc.PLL.PLLN = 160;
  osc.PLL.PLLFRACN = 0;
  osc.PLL.PLLP = 2;
  osc.PLL.PLLR = 2;
  osc.PLL.PLLQ = 4;
  osc.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  osc.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;
  if (HAL_RCC_OscConfig(&osc) != HAL_OK) Error_Handler();

  clk.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_D1PCLK1 |
                  RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2 | RCC_CLOCKTYPE_D3PCLK1;
  clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clk.SYSCLKDivider = RCC_SYSCLK_DIV1;
  clk.AHBCLKDivider = RCC_HCLK_DIV2;
  clk.APB3CLKDivider = RCC_APB3_DIV2;
  clk.APB1CLKDivider = RCC_APB1_DIV2;
  clk.APB2CLKDivider = RCC_APB2_DIV2;
  clk.APB4CLKDivider = RCC_APB4_DIV2;
  if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_4) != HAL_OK) Error_Handler();
}

/* .axisram_bss in the linker script: zero-initialised data placed in AXI SRAM. */
extern uint8_t _saxisram_bss[], _eaxisram_bss[];

void board_init(void)
{
  memset(_saxisram_bss, 0, (size_t)(_eaxisram_bss - _saxisram_bss));
  mpu_config();
  SCB_EnableICache();
  SCB_EnableDCache();
  HAL_Init();
  clock_config();
  BSP_LED_Init(LED_GREEN);
  BSP_LED_Init(LED_ORANGE);
  BSP_LED_Init(LED_RED);
  BSP_LED_Init(LED_BLUE);
}

void board_console_init(uint32_t baud)
{
  COM_InitTypeDef com = {
    .BaudRate = baud,
    .WordLength = COM_WORDLENGTH_8B,
    .StopBits = COM_STOPBITS_1,
    .Parity = COM_PARITY_NONE,
    .HwFlowCtl = COM_HWCONTROL_NONE,
  };
  if (BSP_COM_Init(COM1, &com) != BSP_ERROR_NONE) Error_Handler();
  BSP_COM_SelectLogPort(COM1);
}

void Error_Handler(void)
{
  __disable_irq();
  BSP_LED_On(LED_RED);
  while (1) {}
}
