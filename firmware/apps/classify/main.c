/* classify: camera -> the DeepGate-compiled classifier -> LCD.
 *
 *   camera (320x240 RGB565)
 *     |-- preprocess(): centre crop to the input's shape, area resize, then
 *     |      scale+quantize (LUT from model_meta.h) -> int8 R/G/B planes
 *     |-- run_model(): libmodel.a from the DeepGate build
 *     '-- camera image, border, label and per-label bars -> LCD
 *           '-- mirror_send(): the finished screen -> tools/mirror.py, while it listens
 *
 * MODEL_DIR (set by ./deploy.sh) holds model.h, libmodel.a and model_meta.h;
 * tasks/<TASK>/labels.h sets how each label looks on screen. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "cam.h"
#include "display.h"
#include "model.h"
#include "model_meta.h"
#include "labels.h"
#include "mirror.h"
#include "stm32_lcd.h"

/* Centre crop to the input's shape, as training does: only columns or only rows are cut. */
#define CROP_WIDE (CAM_W * MODEL_META_IN_H > CAM_H * MODEL_META_IN_W)
#define CROP_W    (CROP_WIDE ? CAM_H * MODEL_META_IN_W / MODEL_META_IN_H : CAM_W)
#define CROP_H    (CROP_WIDE ? CAM_H : CAM_W * MODEL_META_IN_H / MODEL_META_IN_W)
#define CROP_X    ((CAM_W - CROP_W) / 2)
#define CROP_Y    ((CAM_H - CROP_H) / 2)
#define FACTOR    (CROP_W / MODEL_META_IN_W)

#if CROP_W != MODEL_META_IN_W * FACTOR || CROP_H != MODEL_META_IN_H * FACTOR
#error "only integer area resize factors are implemented"
#endif
#if MODEL_META_IN_C != 3 || MODEL_META_IN_C * MODEL_META_IN_H * MODEL_META_IN_W != MODEL_INPUT0_ELEMENTS
#error "model_meta.h and model.h describe different inputs"
#endif
#if MODEL_META_LABEL_COUNT != MODEL_OUTPUT0_ELEMENTS
#error "model_meta.h and model.h disagree on the number of labels"
#endif

/* Probabilities are averaged over frames so the result doesn't flicker:
 * p = SMOOTHING * p + (1 - SMOOTHING) * p_new. 0 turns it off. */
#define SMOOTHING 0.5f

static const char *label_text[MODEL_META_LABEL_COUNT];
static uint32_t label_colour[MODEL_META_LABEL_COUNT];

static void styles_init(void)
{
  for (int i = 0; i < MODEL_META_LABEL_COUNT; i++) {
    label_text[i] = MODEL_META_LABELS[i];
    label_colour[i] = UTIL_LCD_COLOR_WHITE;
    for (unsigned s = 0; s < sizeof LABEL_STYLES / sizeof LABEL_STYLES[0]; s++)
      if (strcmp(LABEL_STYLES[s].label, MODEL_META_LABELS[i]) == 0) {
        label_text[i] = LABEL_STYLES[s].text;
        label_colour[i] = LABEL_STYLES[s].colour;
      }
  }
}

/* ---- timing: the Cortex-M7 cycle counter (DWT) ---- */
static void cycle_counter_init(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->LAR = 0xC5ACCE55;  /* unlock the DWT registers (M7) */
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static float cycles_to_ms(uint32_t cycles) { return cycles / (SystemCoreClock / 1000.0f); }

/* ---- preprocessing --------------------------------------------------------------
 * For every input pixel: widen its FACTOR x FACTOR block of RGB565 pixels to 8 bits
 * (bit replication, as tools/capture_link.py saves training images), average each
 * channel rounding half up (training's area resize), then look up the int8 value.
 * The model takes channel planes: R, then G, then B. */
static void preprocess(const uint16_t *frame, int8_t *input)
{
  enum { F = FACTOR, H = MODEL_META_IN_H, W = MODEL_META_IN_W };
  int8_t *plane_r = input, *plane_g = input + H * W, *plane_b = input + 2 * H * W;
  for (uint32_t y = 0; y < H; y++) {
    const uint16_t *block_row = frame + (CROP_Y + y * F) * CAM_W + CROP_X;
    for (uint32_t x = 0; x < W; x++) {
      uint32_t sum_r = 0, sum_g = 0, sum_b = 0;
      for (uint32_t dy = 0; dy < F; dy++) {
        const uint16_t *p = block_row + dy * CAM_W + x * F;
        for (uint32_t dx = 0; dx < F; dx++) {
          uint32_t v = p[dx], r = v >> 11, g = (v >> 5) & 0x3F, b = v & 0x1F;
          sum_r += r << 3 | r >> 2;
          sum_g += g << 2 | g >> 4;
          sum_b += b << 3 | b >> 2;
        }
      }
      plane_r[y * W + x] = MODEL_META_INPUT_LUT[(sum_r + F * F / 2) / (F * F)];
      plane_g[y * W + x] = MODEL_META_INPUT_LUT[(sum_g + F * F / 2) / (F * F)];
      plane_b[y * W + x] = MODEL_META_INPUT_LUT[(sum_b + F * F / 2) / (F * F)];
    }
  }
}

/* Dequantize the int8 logits, then softmax into probs[]. */
static void softmax(const int8_t *logits, float *probs)
{
  float l[MODEL_META_LABEL_COUNT], max = -INFINITY, sum = 0;
  for (int i = 0; i < MODEL_META_LABEL_COUNT; i++) {
    l[i] = (logits[i] - MODEL_META_OUT_ZERO_POINT) * MODEL_META_OUT_SCALE;
    if (l[i] > max) max = l[i];
  }
  for (int i = 0; i < MODEL_META_LABEL_COUNT; i++) sum += probs[i] = expf(l[i] - max);
  for (int i = 0; i < MODEL_META_LABEL_COUNT; i++) probs[i] /= sum;
}

/* ---- display --------------------------------------------------------------------- */
/* One row per label: its name and percentage, and a bar filled to the probability. */
static void draw_bars(uint32_t x, uint32_t y, const float *probs, int best)
{
  enum { BAR_W = 136, BAR_H = 12, ROW_H = 40 };
  char line[24];
  UTIL_LCD_SetFont(&Font12);
  for (int i = 0; i < MODEL_META_LABEL_COUNT; i++, y += ROW_H) {
    UTIL_LCD_SetTextColor(i == best ? label_colour[i] : UTIL_LCD_COLOR_GRAY);
    snprintf(line, sizeof line, "%-14.14s%3d%%", label_text[i], (int)(probs[i] * 100.0f + 0.5f));
    UTIL_LCD_DisplayStringAt(x, y, (uint8_t *)line, LEFT_MODE);
    uint32_t filled = (uint32_t)(probs[i] * BAR_W + 0.5f);
    UTIL_LCD_FillRect(x, y + 16, BAR_W, BAR_H, 0xFF303030);
    if (filled) UTIL_LCD_FillRect(x, y + 16, filled, BAR_H, label_colour[i]);
  }
}

int main(void)
{
  board_init();
  mirror_init();
  cycle_counter_init();
  styles_init();
  printf("\r\nclassify: %s\r\n", TASK_TITLE);
  printf("model: %s, input %dx%dx%d int8, %d labels\r\n", MODEL_TARGET_ID, MODEL_META_IN_W,
         MODEL_META_IN_H, MODEL_META_IN_C, MODEL_META_LABEL_COUNT);

  if (display_init() != 0) {
    printf("display init failed\r\n");
    Error_Handler();
  }
  UTIL_LCD_Clear(UTIL_LCD_COLOR_BLACK);
  if (cam_start() != 0) Error_Handler();

  const uint32_t img_w = 2 * CAM_W, img_h = 2 * CAM_H;
  const uint32_t panel_x = img_w + 8;
  char line[24];
  UTIL_LCD_SetBackColor(UTIL_LCD_COLOR_BLACK);
  UTIL_LCD_SetFont(&Font16);
  UTIL_LCD_SetTextColor(UTIL_LCD_COLOR_GRAY);
  UTIL_LCD_DisplayStringAt(panel_x, 16, (uint8_t *)TASK_TITLE, LEFT_MODE);
  snprintf(line, sizeof line, "%dx%d RGB", MODEL_META_IN_W, MODEL_META_IN_H);
  UTIL_LCD_DisplayStringAt(panel_x, 36, (uint8_t *)line, LEFT_MODE);
  display_show();

  int8_t *input = model_get_input0();
  float probs[MODEL_META_LABEL_COUNT], smoothed[MODEL_META_LABEL_COUNT];
  for (int i = 0; i < MODEL_META_LABEL_COUNT; i++) smoothed[i] = 1.0f / MODEL_META_LABEL_COUNT;
  uint32_t shown = 0, frames_last = 0, t_last = HAL_GetTick();
  float prep_ms = 0, infer_ms = 0;
  /* The once-a-second status line, held until no mirror frame is going out. */
  char status[200] = "";
  while (1) {
    const uint16_t *frame = cam_wait_frame();

    uint32_t c0 = DWT->CYCCNT;
    preprocess(frame, input);
    uint32_t c1 = DWT->CYCCNT;
    display_wait();
    display_draw_rgb565_2x(frame, CAM_W, CAM_H, 0, 0);
    uint32_t c2 = DWT->CYCCNT;
    if (run_model() != 0) {
      printf("run_model failed\r\n");
      Error_Handler();
    }
    uint32_t c3 = DWT->CYCCNT;
    prep_ms = cycles_to_ms(c1 - c0);
    infer_ms = cycles_to_ms(c3 - c2);

    softmax(model_get_output0(), probs);
    int best = 0;
    for (int i = 0; i < MODEL_META_LABEL_COUNT; i++) {
      smoothed[i] = SMOOTHING * smoothed[i] + (1.0f - SMOOTHING) * probs[i];
      if (smoothed[i] > smoothed[best]) best = i;
    }
    display_draw_border(0, 0, img_w, img_h, 10, label_colour[best]);

    UTIL_LCD_SetFont(&Font20);
    UTIL_LCD_SetTextColor(label_colour[best]);
    snprintf(line, sizeof line, "%-10.10s", label_text[best]);
    UTIL_LCD_DisplayStringAt(panel_x, 80, (uint8_t *)line, LEFT_MODE);
    UTIL_LCD_SetFont(&Font24);
    snprintf(line, sizeof line, "%3d%%  ", (int)(smoothed[best] * 100.0f + 0.5f));
    UTIL_LCD_DisplayStringAt(panel_x, 106, (uint8_t *)line, LEFT_MODE);
    draw_bars(panel_x, 160, smoothed, best);

    UTIL_LCD_SetFont(&Font16);
    UTIL_LCD_SetTextColor(UTIL_LCD_COLOR_GRAY);
    snprintf(line, sizeof line, "infer %5.1f ms", (double)infer_ms);
    UTIL_LCD_DisplayStringAt(panel_x, 400, (uint8_t *)line, LEFT_MODE);
    snprintf(line, sizeof line, "prep  %5.1f ms", (double)prep_ms);
    UTIL_LCD_DisplayStringAt(panel_x, 420, (uint8_t *)line, LEFT_MODE);
    shown++;

    if (HAL_GetTick() - t_last >= 1000) {
      uint32_t frames = cam_frames();
      snprintf(line, sizeof line, "%2lu fps (cam %2lu)", shown, frames - frames_last);
      UTIL_LCD_DisplayStringAt(panel_x, 440, (uint8_t *)line, LEFT_MODE);
      int n = snprintf(status, sizeof status, "%s", MODEL_META_LABELS[best]);
      for (int i = 0; i < MODEL_META_LABEL_COUNT && n < (int)sizeof status; i++)
        n += snprintf(status + n, sizeof status - n, " %s=%.2f", MODEL_META_LABELS[i], (double)smoothed[i]);
      if (n < (int)sizeof status)
        snprintf(status + n, sizeof status - n, " | infer %.1f ms | prep %.1f ms | %lu fps (camera %lu)\r\n",
                 (double)infer_ms, (double)prep_ms, shown, frames - frames_last);
      frames_last = frames;
      shown = 0;
      t_last += 1000;
      BSP_LED_Toggle(LED_GREEN);
    }
    display_show();

    /* Text only between mirror frames, never into one. */
    if (status[0] && mirror_idle()) {
      printf("%s", status);
      status[0] = 0;
    }
    mirror_send();
  }
}
