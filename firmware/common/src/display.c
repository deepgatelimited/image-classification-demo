/* display: 800x480 DSI panel in adapted command mode with tearing-effect sync.
 * Adapted from ST's STM32H747I-DISCO DCMI_CaptureMode example (STM32CubeH7),
 * with the panel controller chosen at runtime instead of at compile time.
 *
 * Why command mode: the panel is natively portrait (480x800) and turned to
 * landscape with MADCTL, so it repaints its glass along our x axis while video
 * mode streams rows down our y axis. The two sweeps cross on a diagonal, which
 * shows as a diagonal tear. In command mode the panel keeps showing its own
 * memory (GRAM) and only receives a new image when we send one. The panel's TE
 * pin tells us when its glass refresh is at a known spot, so we start writing
 * there and stay ahead of it.
 *
 * How a refresh goes:
 *   display_show()      unmask the TE pin (PJ2 alternate function = DSI_TE)
 *   TE interrupt        mask TE again; HAL_DSI_Refresh() sends the LEFT half
 *   end-of-refresh IRQ  point the LTDC at the right half; send the RIGHT half
 *   end-of-refresh IRQ  point the LTDC back at the left half; done
 * The LTDC is set up as a 400-pixel-wide window with an 800-pixel pitch, so
 * each refresh carries one half (DSI command size is limited); CASET tells the
 * panel which columns the incoming half lands in. */
#include "display.h"
#include "stm32h747i_discovery_bus.h"
#include "stm32h747i_discovery_lcd.h"
#include "stm32h747i_discovery_sdram.h"
#include "stm32_lcd.h"
#include "../Components/nt35510/nt35510_reg.h"
#include "../Components/otm8009a/otm8009a_reg.h"

#define HALF_W (DISPLAY_W / 2)

/* LTDC "timings". In command mode there is no real video timing; the LTDC just
 * hands one half-frame to the DSI wrapper per refresh, so sync and porches are
 * minimal. */
#define HSYNC 1
#define HBP   1
#define HFP   1
#define VSYNC 1
#define VBP   1
#define VFP   1

/* MIPI DCS column/row address set: same codes on both panels. */
#define DCS_CASET 0x2A
#define DCS_RASET 0x2B

/* Routing PJ2 to DSI (AF13) lets TE pulses through; AF0 blocks them. */
#define TE_UNMASK() (GPIOJ->AFR[0] |= ((uint32_t)GPIO_AF13_DSI << 8))
#define TE_MASK()   (GPIOJ->AFR[0] &= ~(0xFU << 8))

enum { LEFT, RIGHT };

static uint8_t cols_left[]  = {0x00, 0x00, 0x01, 0x8F};  /*   0 .. 399 */
static uint8_t cols_right[] = {0x01, 0x90, 0x03, 0x1F};  /* 400 .. 799 */
static uint8_t rows_all[]   = {0x00, 0x00, 0x01, 0xDF};  /*   0 .. 479 */

static volatile int refresh_pending;
static volatile int area;              /* half the current refresh is sending */
static int panel_nt35510;

/* ---- DSI register access for the panel drivers (from ST's example) -------- */
static int32_t dsi_io_write(uint16_t ch, uint16_t reg, uint8_t *data, uint16_t size)
{
  HAL_StatusTypeDef s = (size <= 1)
    ? HAL_DSI_ShortWrite(&hlcd_dsi, ch, DSI_DCS_SHORT_PKT_WRITE_P1, reg, (uint32_t)data[size])
    : HAL_DSI_LongWrite(&hlcd_dsi, ch, DSI_DCS_LONG_PKT_WRITE, size, (uint32_t)reg, data);
  return s == HAL_OK ? BSP_ERROR_NONE : BSP_ERROR_BUS_FAILURE;
}

static int32_t dsi_io_read(uint16_t ch, uint16_t reg, uint8_t *data, uint16_t size)
{
  return HAL_DSI_Read(&hlcd_dsi, ch, data, size, DSI_DCS_SHORT_PKT_READ, reg, data) == HAL_OK
         ? BSP_ERROR_NONE : BSP_ERROR_BUS_FAILURE;
}

/* Panel commands can go over DSI in low-power mode (slow, robust: used while
 * setting the panel up) or high-speed mode (needed for pixel data). All the
 * *_DISABLE values are 0, so a zeroed struct means "everything high-speed". */
static void dsi_commands_low_power(int on)
{
  DSI_LPCmdTypeDef c = {0};
  if (on) {
    c.LPGenShortWriteNoP  = DSI_LP_GSW0P_ENABLE;
    c.LPGenShortWriteOneP = DSI_LP_GSW1P_ENABLE;
    c.LPGenShortWriteTwoP = DSI_LP_GSW2P_ENABLE;
    c.LPGenShortReadNoP   = DSI_LP_GSR0P_ENABLE;
    c.LPGenShortReadOneP  = DSI_LP_GSR1P_ENABLE;
    c.LPGenShortReadTwoP  = DSI_LP_GSR2P_ENABLE;
    c.LPGenLongWrite      = DSI_LP_GLW_ENABLE;
    c.LPDcsShortWriteNoP  = DSI_LP_DSW0P_ENABLE;
    c.LPDcsShortWriteOneP = DSI_LP_DSW1P_ENABLE;
    c.LPDcsShortReadNoP   = DSI_LP_DSR0P_ENABLE;
    c.LPDcsLongWrite      = DSI_LP_DLW_ENABLE;
  }
  HAL_DSI_ConfigCommand(&hlcd_dsi, &c);
}

/* Clocks and resets for LTDC, DMA2D, DSI. Only the DSI interrupt is enabled:
 * it carries the TE and end-of-refresh events. (ST's example also enables the
 * LTDC and DMA2D interrupts without handlers; we poll DMA2D and leave them off
 * so a stray LTDC error can't land in Default_Handler.) */
static void msp_init(void)
{
  __HAL_RCC_LTDC_CLK_ENABLE();
  __HAL_RCC_LTDC_FORCE_RESET();
  __HAL_RCC_LTDC_RELEASE_RESET();
  __HAL_RCC_DMA2D_CLK_ENABLE();
  __HAL_RCC_DMA2D_FORCE_RESET();
  __HAL_RCC_DMA2D_RELEASE_RESET();
  __HAL_RCC_DSI_CLK_ENABLE();
  __HAL_RCC_DSI_FORCE_RESET();
  __HAL_RCC_DSI_RELEASE_RESET();
  HAL_NVIC_SetPriority(DSI_IRQn, 9, 0);
  HAL_NVIC_EnableIRQ(DSI_IRQn);
}

static void ltdc_init(void)
{
  hlcd_ltdc.Instance = LTDC;
  HAL_LTDC_DeInit(&hlcd_ltdc);
  hlcd_ltdc.Init.HorizontalSync     = HSYNC;
  hlcd_ltdc.Init.VerticalSync       = VSYNC;
  hlcd_ltdc.Init.AccumulatedHBP     = HSYNC + HBP;
  hlcd_ltdc.Init.AccumulatedVBP     = VSYNC + VBP;
  hlcd_ltdc.Init.AccumulatedActiveW = HSYNC + HBP + HALF_W;
  hlcd_ltdc.Init.AccumulatedActiveH = VSYNC + VBP + DISPLAY_H;
  hlcd_ltdc.Init.TotalWidth         = HSYNC + HBP + HALF_W + HFP;
  hlcd_ltdc.Init.TotalHeigh         = VSYNC + VBP + DISPLAY_H + VFP;
  hlcd_ltdc.Init.Backcolor.Red = hlcd_ltdc.Init.Backcolor.Green = hlcd_ltdc.Init.Backcolor.Blue = 0;
  hlcd_ltdc.Init.HSPolarity = LTDC_HSPOLARITY_AL;
  hlcd_ltdc.Init.VSPolarity = LTDC_VSPOLARITY_AL;
  hlcd_ltdc.Init.DEPolarity = LTDC_DEPOLARITY_AL;
  hlcd_ltdc.Init.PCPolarity = LTDC_PCPOLARITY_IPC;
  HAL_LTDC_Init(&hlcd_ltdc);
}

/* One LTDC layer: a 400 x 480 window over an 800-pixel-pitch framebuffer. */
static void ltdc_layer_init(void)
{
  LTDC_LayerCfgTypeDef l = {0};
  l.WindowX0 = 0;
  l.WindowX1 = HALF_W;
  l.WindowY0 = 0;
  l.WindowY1 = DISPLAY_H;
  l.PixelFormat = LTDC_PIXEL_FORMAT_ARGB8888;
  l.FBStartAdress = LCD_LAYER_0_ADDRESS;
  l.Alpha = 255;
  l.Alpha0 = 0;
  l.BlendingFactor1 = LTDC_BLENDING_FACTOR1_PAxCA;
  l.BlendingFactor2 = LTDC_BLENDING_FACTOR2_PAxCA;
  l.ImageWidth = HALF_W;
  l.ImageHeight = DISPLAY_H;
  HAL_LTDC_ConfigLayer(&hlcd_ltdc, &l, 0);
  HAL_LTDC_SetPitch(&hlcd_ltdc, DISPLAY_W, 0);
}

/* Reads the controller ID over DSI and runs its init sequence (sleep out,
 * pixel format, MADCTL landscape, display on). */
static int panel_init(void)
{
  static NT35510_Object_t nt;
  static OTM8009A_Object_t otm;
  NT35510_IO_t nt_io = {.Address = 0, .GetTick = BSP_GetTick, .WriteReg = dsi_io_write, .ReadReg = dsi_io_read};
  OTM8009A_IO_t otm_io = {.Address = 0, .GetTick = BSP_GetTick, .WriteReg = dsi_io_write, .ReadReg = dsi_io_read};
  uint32_t id = 0;

  if (NT35510_RegisterBusIO(&nt, &nt_io) == NT35510_OK &&
      NT35510_ReadID(&nt, &id) == NT35510_OK && id == NT35510_ID) {
    panel_nt35510 = 1;
    return NT35510_Init(&nt, NT35510_COLMOD_RGB888, NT35510_ORIENTATION_LANDSCAPE) == NT35510_OK ? 0 : -1;
  }
  if (OTM8009A_RegisterBusIO(&otm, &otm_io) != OTM8009A_OK) return -1;
  return OTM8009A_Init(&otm, OTM8009A_COLMOD_RGB888, OTM8009A_ORIENTATION_LANDSCAPE) == OTM8009A_OK ? 0 : -1;
}

int display_init(void)
{
  if (BSP_SDRAM_Init(0) != BSP_ERROR_NONE) return -1;

  BSP_LCD_Reset(0);  /* panel XRES pulse */
  msp_init();

  /* LTDC pixel clock from PLL3: 25 MHz / M5 * N160 / R19 = 42 MHz. */
  RCC_PeriphCLKInitTypeDef clk = {0};
  clk.PeriphClockSelection = RCC_PERIPHCLK_LTDC;
  clk.PLL3.PLL3M = 5;
  clk.PLL3.PLL3N = 160;
  clk.PLL3.PLL3P = 2;
  clk.PLL3.PLL3Q = 2;
  clk.PLL3.PLL3R = 19;
  clk.PLL3.PLL3VCOSEL = RCC_PLL3VCOWIDE;
  clk.PLL3.PLL3RGE = RCC_PLL3VCIRANGE_2;
  if (HAL_RCCEx_PeriphCLKConfig(&clk) != HAL_OK) return -2;

  /* DSI host: 2 data lanes; DSI PLL 25 MHz / IDF5 * NDIV100 * 2 / ODF1 = 1 GHz,
   * i.e. 500 Mbit/s per lane. */
  hlcd_dsi.Instance = DSI;
  HAL_DSI_DeInit(&hlcd_dsi);
  DSI_PLLInitTypeDef pll = {.PLLNDIV = 100, .PLLIDF = DSI_PLL_IN_DIV5, .PLLODF = DSI_PLL_OUT_DIV1};
  hlcd_dsi.Init.NumberOfLanes = DSI_TWO_DATA_LANES;
  hlcd_dsi.Init.TXEscapeCkdiv = 0x4;
  if (HAL_DSI_Init(&hlcd_dsi, &pll) != HAL_OK) return -3;

  /* Adapted command mode: the LTDC feeds the DSI host, but pixels only flow when
   * HAL_DSI_Refresh() is called. TE comes in on the external pin. */
  DSI_CmdCfgTypeDef cmd = {0};
  cmd.VirtualChannelID      = 0;
  cmd.HSPolarity            = DSI_HSYNC_ACTIVE_HIGH;
  cmd.VSPolarity            = DSI_VSYNC_ACTIVE_HIGH;
  cmd.DEPolarity            = DSI_DATA_ENABLE_ACTIVE_HIGH;
  cmd.ColorCoding           = DSI_RGB888;
  cmd.CommandSize           = HALF_W;
  cmd.TearingEffectSource   = DSI_TE_EXTERNAL;
  cmd.TearingEffectPolarity = DSI_TE_RISING_EDGE;
  cmd.VSyncPol              = DSI_VSYNC_FALLING;
  cmd.AutomaticRefresh      = DSI_AR_DISABLE;
  cmd.TEAcknowledgeRequest  = DSI_TE_ACKNOWLEDGE_DISABLE;
  if (HAL_DSI_ConfigAdaptedCommandMode(&hlcd_dsi, &cmd) != HAL_OK) return -4;

  dsi_commands_low_power(1);

  ltdc_init();
  HAL_DSI_Start(&hlcd_dsi);

  DSI_PHY_TimerTypeDef phy = {
    .ClockLaneHS2LPTime = 35, .ClockLaneLP2HSTime = 35,
    .DataLaneHS2LPTime = 35, .DataLaneLP2HSTime = 35,
    .DataLaneMaxReadTime = 0, .StopWaitTime = 10,
  };
  HAL_DSI_ConfigPhyTimer(&hlcd_dsi, &phy);

  /* Reading the panel ID needs bus turn-around (the panel talks back). */
  HAL_DSI_ConfigFlowControl(&hlcd_dsi, DSI_FLOW_CONTROL_BTA);
  if (panel_init() != 0) return -5;

  dsi_commands_low_power(0);
  HAL_DSI_ForceRXLowPower(&hlcd_dsi, ENABLE);

  /* TE pin: PJ2. Starts masked (AF0) until the first display_show(). */
  __HAL_RCC_GPIOJ_CLK_ENABLE();
  GPIO_InitTypeDef te = {.Pin = GPIO_PIN_2, .Mode = GPIO_MODE_AF_PP, .Pull = GPIO_NOPULL,
                         .Speed = GPIO_SPEED_FREQ_HIGH, .Alternate = GPIO_AF13_DSI};
  HAL_GPIO_Init(GPIOJ, &te);
  TE_MASK();

  /* Tell the BSP drawing functions (BSP_LCD_FillRect etc., used by UTIL_LCD)
   * what the framebuffer looks like; BSP_LCD_Init would normally do this. */
  Lcd_Ctx[0].ActiveLayer = 0;
  Lcd_Ctx[0].PixelFormat = LCD_PIXEL_FORMAT_ARGB8888;
  Lcd_Ctx[0].BppFactor = 4;
  Lcd_Ctx[0].XSize = DISPLAY_W;
  Lcd_Ctx[0].YSize = DISPLAY_H;

  /* The LTDC may only be reconfigured while the DSI wrapper is off. */
  __HAL_DSI_WRAPPER_DISABLE(&hlcd_dsi);
  ltdc_layer_init();
  __HAL_DSI_WRAPPER_ENABLE(&hlcd_dsi);

  /* Panel address window for the first (left) half, and TE output. */
  HAL_DSI_LongWrite(&hlcd_dsi, 0, DSI_DCS_LONG_PKT_WRITE, 4, DCS_CASET, cols_left);
  HAL_DSI_LongWrite(&hlcd_dsi, 0, DSI_DCS_LONG_PKT_WRITE, 4, DCS_RASET, rows_all);
  if (panel_nt35510) {
    static uint8_t te_on[] = {0x01, 0x60};
    HAL_DSI_LongWrite(&hlcd_dsi, 0, DSI_DCS_LONG_PKT_WRITE, 2, NT35510_CMD_TEEON, te_on);
  } else {
    static uint8_t te_scanline[] = {0x02, 0x15};  /* TE at scan line 533 */
    HAL_DSI_LongWrite(&hlcd_dsi, 0, DSI_DCS_LONG_PKT_WRITE, 2, OTM8009A_CMD_WRTESCN, te_scanline);
  }

  UTIL_LCD_SetFuncDriver(&LCD_Driver);
  UTIL_LCD_SetLayer(0);
  area = LEFT;
  return 0;
}

uint32_t *display_framebuffer(void) { return (uint32_t *)LCD_LAYER_0_ADDRESS; }

void display_draw_rgb565_2x(const uint16_t *src, uint32_t w, uint32_t h, uint32_t x, uint32_t y)
{
  uint32_t *fb = display_framebuffer();
  for (uint32_t sy = 0; sy < h; sy++) {
    uint32_t *row0 = fb + (y + 2 * sy) * DISPLAY_W + x;
    uint32_t *row1 = row0 + DISPLAY_W;
    const uint16_t *in = src + sy * w;
    for (uint32_t sx = 0; sx < w; sx++) {
      uint32_t p = in[sx];
      uint32_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
      /* 5/6 bits to 8 by bit replication (31 -> 255), as tools/capture_link.py does. */
      uint32_t argb = 0xFF000000 | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
      row0[2 * sx] = row0[2 * sx + 1] = argb;
      row1[2 * sx] = row1[2 * sx + 1] = argb;
    }
  }
}

void display_draw_border(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t t, uint32_t colour)
{
  UTIL_LCD_FillRect(x, y, w, t, colour);
  UTIL_LCD_FillRect(x, y + h - t, w, t, colour);
  UTIL_LCD_FillRect(x, y, t, h, colour);
  UTIL_LCD_FillRect(x + w - t, y, t, h, colour);
}

void display_show(void)
{
  refresh_pending = 1;
  TE_UNMASK();
}

void display_wait(void)
{
  while (refresh_pending) {}
}

/* ---- interrupts ------------------------------------------------------------ */
void DSI_IRQHandler(void) { HAL_DSI_IRQHandler(&hlcd_dsi); }

/* The panel's glass refresh reached the TE scan line: start sending. */
void HAL_DSI_TearingEffectCallback(DSI_HandleTypeDef *hdsi)
{
  TE_MASK();
  HAL_DSI_Refresh(hdsi);
}

/* One half has been sent. After the left half, send the right; after the right,
 * reset to the left for next time and mark the refresh done. */
void HAL_DSI_EndOfRefreshCallback(DSI_HandleTypeDef *hdsi)
{
  if (!refresh_pending) return;
  __HAL_DSI_WRAPPER_DISABLE(hdsi);
  if (area == LEFT) {
    LTDC_LAYER(&hlcd_ltdc, 0)->CFBAR = LCD_LAYER_0_ADDRESS + HALF_W * 4;
    __HAL_LTDC_RELOAD_CONFIG(&hlcd_ltdc);
    __HAL_DSI_WRAPPER_ENABLE(hdsi);
    HAL_DSI_LongWrite(hdsi, 0, DSI_DCS_LONG_PKT_WRITE, 4, DCS_CASET, cols_right);
    area = RIGHT;
    HAL_DSI_Refresh(hdsi);
  } else {
    LTDC_LAYER(&hlcd_ltdc, 0)->CFBAR = LCD_LAYER_0_ADDRESS;
    __HAL_LTDC_RELOAD_CONFIG(&hlcd_ltdc);
    __HAL_DSI_WRAPPER_ENABLE(hdsi);
    HAL_DSI_LongWrite(hdsi, 0, DSI_DCS_LONG_PKT_WRITE, 4, DCS_CASET, cols_left);
    area = LEFT;
    refresh_pending = 0;
  }
}
