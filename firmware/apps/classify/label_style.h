/* How a label looks on the classify screen. Each task's tasks/<task>/labels.h
 * lists its labels with one of these; the firmware matches them to the model's
 * labels by name, so the order doesn't matter and unlisted labels show in white. */
#ifndef LABEL_STYLE_H
#define LABEL_STYLE_H

#include <stdint.h>
#include "stm32_lcd.h"

struct label_style {
  const char *label;  /* the folder name in the dataset, as in model_spec.json */
  const char *text;   /* what the screen says (up to 10 characters fit) */
  uint32_t colour;    /* border and text colour, ARGB8888 (UTIL_LCD_COLOR_*) */
};

#endif
