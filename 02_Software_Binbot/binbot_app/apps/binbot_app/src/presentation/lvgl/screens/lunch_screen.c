#include "../ui_internal.h"
#include "binbot/platform.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct {
    const char *name;
    int category, price, minutes, calories;
} dish_t;
static const dish_t dishes[] = {
    {"番茄牛腩饭", 1, 32, 25, 680}, {"黄焖鸡米饭", 1, 26, 20, 720},
    {"咖喱鸡饭", 1, 28, 20, 650}, {"兰州牛肉面", 2, 22, 15, 560},
    {"葱油拌面", 2, 16, 10, 520}, {"酸辣汤面", 2, 24, 20, 590},
    {"鸡胸肉沙拉", 3, 35, 15, 380}, {"牛油果鸡肉碗", 3, 36, 15, 450},
    {"田园蔬菜沙拉", 3, 30, 10, 320}, {"日式定食", 4, 45, 30, 700},
    {"石锅拌饭", 4, 36, 25, 640}, {"豚骨拉面", 4, 42, 25, 780},
};
static const char *categories[] = {"全部", "米饭", "面食", "轻食", "日韩"};
typedef struct lunch_context lunch_context_t;
typedef struct { lunch_context_t *context; int action; } lunch_binding_t;
struct lunch_context {
    ui_app_t *app;
    lv_obj_t *hero, *caption, *name, *details, *roll, *roll_label, *pick, *exclude;
    lv_obj_t *count, *restore, *budget_label, *category_buttons[5], *category_labels[5];
    lv_obj_t *budgets[4], *budget_labels[4];
    lunch_binding_t bindings[13];
    lv_timer_t *timer;
    uint32_t random;
    uint16_t skipped;
    int category, budget, current, ticks, pool[12], pool_count;
    bool rolling, picked;
};

static int pool_build(lunch_context_t *c, int *pool)
{
    int count = 0;
    for(int i = 0; i < 12; ++i)
        if((!c->category || dishes[i].category == c->category) &&
           dishes[i].price <= c->budget && !(c->skipped & (1U << i))) pool[count++] = i;
    return count;
}

static void lunch_sync(lunch_context_t *c)
{
    int pool[12], count = pool_build(c, pool);
    bool empty = count == 0;
    char surface[20], text[256];
    snprintf(surface, sizeof(surface), "dish%d", c->current);
    ui_ref_surface(c->hero, empty ? "lunch_empty" : surface);
    lv_label_set_text(c->caption, c->picked ? "就决定是你了" : c->rolling ? "正在挑选…" : "今天中午吃");
    lv_label_set_text(c->name, empty ? "没有符合条件的" : dishes[c->current].name);
    lv_obj_set_style_text_opa(c->name, c->rolling ? 128 : 255, 0);
    const dish_t *dish = &dishes[c->current];
    snprintf(text, sizeof(text), "%s · ¥%d · 约 %d 分钟 · %d kcal", categories[dish->category], dish->price, dish->minutes, dish->calories);
    lv_label_set_text(c->details, text);
    lv_obj_set_flag(c->details, LV_OBJ_FLAG_HIDDEN, empty);
    lv_obj_set_flag(c->pick, LV_OBJ_FLAG_HIDDEN, empty || c->rolling || c->picked);
    lv_obj_set_flag(c->exclude, LV_OBJ_FLAG_HIDDEN, empty || c->rolling);
    lv_obj_set_state(c->roll, LV_STATE_DISABLED, empty || c->rolling);
    lv_obj_set_style_opa(c->roll, empty ? 102 : 255, 0);
    lv_label_set_text(c->roll_label, c->picked ? "再换一个" : "帮我选");
    lv_obj_set_width(c->roll, c->picked ? 140 : 121);
    if(c->skipped) {
        size_t used = (size_t)snprintf(text, sizeof(text), "已排除 ");
        for(int i = 0; i < 12; ++i) {
            if(!(c->skipped & (1U << i))) continue;
            used += (size_t)snprintf(text + used, sizeof(text) - used, "%s%s", used > 10 ? "、" : "", dishes[i].name);
            if(used >= sizeof(text) - 48) break;
        }
    }
    else snprintf(text, sizeof(text), "共 %d 道可选", count);
    lv_label_set_text(c->count, text);
    lv_obj_set_width(c->count, c->skipped ? 282 : 330);
    lv_obj_set_flag(c->restore, LV_OBJ_FLAG_HIDDEN, c->skipped == 0);
    snprintf(text, sizeof(text), "¥%d", c->budget);
    lv_label_set_text(c->budget_label, text);
    for(int i = 0; i < 5; ++i) {
        lv_obj_set_style_bg_color(c->category_buttons[i], lv_color_hex(c->category == i ? UI_REF_INK : 0xe6e6e6), 0);
        lv_obj_set_style_text_color(c->category_labels[i], lv_color_hex(c->category == i ? 0xffffff : UI_REF_INK), 0);
    }
    for(int i = 0; i < 4; ++i) {
        bool selected = c->budget == (i + 2) * 10;
        lv_obj_set_style_bg_color(c->budgets[i], lv_color_hex(selected ? UI_REF_CLAY : 0xe6e6e6), 0);
        lv_obj_set_style_text_color(c->budget_labels[i], lv_color_hex(selected ? 0xffffff : UI_REF_INK), 0);
    }
}

static void roll_tick(lv_timer_t *timer)
{
    lunch_context_t *c = lv_timer_get_user_data(timer);
    c->random ^= c->random << 13;
    c->random ^= c->random >> 17;
    c->random ^= c->random << 5;
    c->current = c->pool[c->random % (uint32_t)c->pool_count];
    if(++c->ticks == 14) {
        c->rolling = false;
        lv_timer_pause(timer);
    }
    else lv_timer_set_period(timer, 50 + c->ticks * 12);
    lunch_sync(c);
}

static void lunch_action(lv_event_t *event)
{
    lunch_binding_t *binding = lv_event_get_user_data(event);
    lunch_context_t *c = binding->context;
    int action = binding->action;
    if(action < 9 && c->rolling) {
        lv_timer_pause(c->timer);
        c->rolling = false;
    }
    if(action < 5) c->category = action;
    else if(action < 9) c->budget = (action - 3) * 10;
    else if(action == 9) {
        c->pool_count = pool_build(c, c->pool);
        if(c->rolling || c->pool_count == 0) return;
        c->rolling = true; c->picked = false; c->ticks = 0;
        lv_timer_set_period(c->timer, 1);
        lv_timer_resume(c->timer);
        lv_timer_ready(c->timer);
    }
    else if(action == 10) c->picked = true;
    else if(action == 11) { c->skipped |= 1U << c->current; c->picked = false; }
    else c->skipped = 0;
    lunch_sync(c);
}

static void lunch_bind(lunch_context_t *c, lv_obj_t *button, int action)
{
    c->bindings[action] = (lunch_binding_t){c, action};
    lv_obj_add_event_cb(button, lunch_action, LV_EVENT_CLICKED, &c->bindings[action]);
}

static void lunch_destroy(void *data)
{
    lunch_context_t *c = data;
    lv_timer_delete(c->timer);
    free(c);
}

static void lunch_pause(void *data, bool paused)
{
    lunch_context_t *c = data;
    if(paused) lv_timer_pause(c->timer);
    else if(c->rolling) lv_timer_resume(c->timer);
}

ui_screen_t ui_lunch_screen_create(ui_app_t *app)
{
    lunch_context_t *c = calloc(1, sizeof(*c));
    if(!c) return (ui_screen_t){0};
    c->app = app; c->budget = 50; c->random = platform_monotonic_ms() | 1U;
    lv_obj_t *root = ui_ref_shell_latest(app, "中午吃什么", "LUNCH");
    c->hero = ui_box(root, 20, 96, 680, 340, 0xfff8f3, 36);
    lv_obj_set_style_clip_corner(c->hero, true, 0);
    ui_ref_shadow(c->hero, 0, 31, 60, -12, 10);
    c->caption = ui_ref_label(c->hero, "", 16, UI_FONT_MEDIUM, UI_REF_CLAY, 28, 28);
    c->name = ui_ref_label(c->hero, "", 46, UI_FONT_SEMIBOLD, UI_REF_INK, 28, 64);
    lv_obj_set_width(c->name, 325);
    c->details = ui_ref_label(c->hero, "", 16, UI_FONT_REGULAR, UI_REF_MUTED, 28, 126);
    c->roll = ui_button(c->hero, 28, 248, 121, 64, UI_REF_CLAY, 32);
    c->roll_label = ui_centered_label(c->roll, "帮我选", ui_ref_font(19, UI_FONT_SEMIBOLD), 0xffffff);
    ui_ref_shadow(c->roll, UI_REF_CLAY, 255, 48, -8, 12);
    lunch_bind(c, c->roll, 9);
    c->pick = ui_button(c->hero, 161, 248, 102, 64, UI_REF_INK, 32);
    ui_centered_label(c->pick, "就它了", ui_ref_font(18, UI_FONT_SEMIBOLD), 0xffffff);
    lunch_bind(c, c->pick, 10);
    c->exclude = ui_button(c->hero, 560, 276, 100, 44, 0xffffff, 22);
    lv_obj_set_style_bg_opa(c->exclude, 217, 0);
    lv_obj_t *x = ui_ref_icon(c->exclude, "x", 16, UI_REF_INK);
    lv_obj_set_pos(x, 16, 14);
    ui_ref_label(c->exclude, "不想吃", 15, UI_FONT_MEDIUM, UI_REF_INK, 38, 13);
    lunch_bind(c, c->exclude, 11);
    lv_obj_t *category = ui_box(root, 20, 456, 370, 244, 0xffffff, 28);
    ui_ref_surface(category, "lunch_category");
    ui_ref_label(category, "想吃哪类", 16, UI_FONT_REGULAR, UI_REF_FAINT, 20, 20);
    for(int i = 0; i < 5; ++i) {
        c->category_buttons[i] = ui_button(category, 20 + (i * 338 + 2) / 5, 56, 60, 48, 0xe6e6e6, 24);
        c->category_labels[i] = ui_centered_label(c->category_buttons[i], categories[i], ui_ref_font(16, UI_FONT_MEDIUM), UI_REF_INK);
        lunch_bind(c, c->category_buttons[i], i);
    }
    c->count = ui_ref_label(category, "", 15, UI_FONT_REGULAR, UI_REF_FAINT, 20, 202);
    lv_label_set_long_mode(c->count, LV_LABEL_LONG_DOT);
    c->restore = ui_button(category, 310, 198, 40, 30, 0, 0);
    lv_obj_set_style_bg_opa(c->restore, 0, 0);
    ui_centered_label(c->restore, "恢复", ui_ref_font(15, UI_FONT_MEDIUM), UI_REF_CLAY);
    lunch_bind(c, c->restore, 12);
    lv_obj_t *budget = ui_box(root, 404, 456, 296, 244, 0xffffff, 28);
    ui_ref_surface(budget, "lunch_budget");
    ui_ref_label(budget, "预算上限", 16, UI_FONT_REGULAR, UI_REF_FAINT, 20, 28);
    c->budget_label = ui_ref_label(budget, "", 30, UI_FONT_SEMIBOLD, UI_REF_INK, 180, 20);
    lv_obj_set_width(c->budget_label, 96);
    lv_obj_set_style_text_align(c->budget_label, LV_TEXT_ALIGN_RIGHT, 0);
    for(int i = 0; i < 4; ++i) {
        char text[8]; snprintf(text, sizeof(text), "%d", (i + 2) * 10);
        c->budgets[i] = ui_button(budget, 20 + i * 66, 176, 58, 48, 0xe6e6e6, 24);
        c->budget_labels[i] = ui_centered_label(c->budgets[i], text, ui_ref_font(16, UI_FONT_MEDIUM), UI_REF_INK);
        lunch_bind(c, c->budgets[i], i + 5);
    }
    c->timer = lv_timer_create(roll_tick, 50, c);
    lv_timer_pause(c->timer);
    lunch_sync(c);
    return (ui_screen_t){root, c, NULL, lunch_destroy, lunch_pause};
}
