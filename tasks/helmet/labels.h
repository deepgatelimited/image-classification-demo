/* helmet: is the person in frame wearing a helmet? Label the dataset with
 * these folder names so the screen picks up the colours. */
#include "label_style.h"

#define TASK_TITLE "helmet"

static const struct label_style LABEL_STYLES[] = {
  {"no_person", "nobody", UTIL_LCD_COLOR_GRAY},
  {"person_helmet", "HELMET", UTIL_LCD_COLOR_GREEN},
  {"person_no_helmet", "NO HELMET", UTIL_LCD_COLOR_RED},
};
