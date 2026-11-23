#ifndef BINBOT_ASSISTANT_ORB_H
#define BINBOT_ASSISTANT_ORB_H

#include "lvgl.h"

typedef struct ui_assistant_orb ui_assistant_orb_t;

ui_assistant_orb_t *ui_assistant_orb_create(lv_obj_t *parent);
void ui_assistant_orb_set_state(ui_assistant_orb_t *orb, unsigned state);
void ui_assistant_orb_set_paused(ui_assistant_orb_t *orb, bool paused);
void ui_assistant_orb_destroy(ui_assistant_orb_t *orb);

#endif
