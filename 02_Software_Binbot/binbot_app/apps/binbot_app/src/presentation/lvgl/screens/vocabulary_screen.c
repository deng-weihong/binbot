#include "../ui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct vocabulary_context vocabulary_context_t;
typedef struct {
    vocabulary_context_t *context;
    app_vocabulary_accent_t accent;
} vocabulary_speak_binding_t;

struct vocabulary_context {
    ui_app_t *app;
    lv_obj_t *progress;
    lv_obj_t *progress_fill;
    lv_obj_t *plan;
    lv_obj_t *card;
    lv_obj_t *badge;
    lv_obj_t *word;
    lv_obj_t *phonetic;
    lv_obj_t *meaning;
    lv_obj_t *example_box;
    lv_obj_t *example;
    lv_obj_t *translation;
    lv_obj_t *answers;
    lv_obj_t *reveal;
    lv_obj_t *complete;
    lv_obj_t *complete_heading;
    lv_obj_t *complete_ring;
    lv_obj_t *complete_count;
    lv_obj_t *complete_stats;
    vocabulary_speak_binding_t speak[2];
    char current_word[321];
    uint16_t attempts;
    bool revealed;
};

static void vocabulary_update(void *user_data, const app_model_t *model, uint32_t changes);

static void vocabulary_answer(lv_event_t *event, app_vocabulary_answer_t grade)
{
    vocabulary_context_t *context = lv_event_get_user_data(event);
    if(!context->revealed) return;
    (void)app_state_vocabulary_answer(context->app->state, grade);
}

static void vocabulary_known(lv_event_t *event)
{
    vocabulary_answer(event, APP_VOCABULARY_ANSWER_KNOWN);
}

static void vocabulary_unknown(lv_event_t *event)
{
    vocabulary_answer(event, APP_VOCABULARY_ANSWER_AGAIN);
}

static void vocabulary_fuzzy(lv_event_t *event)
{
    vocabulary_answer(event, APP_VOCABULARY_ANSWER_FUZZY);
}

static void vocabulary_reveal(lv_event_t *event)
{
    vocabulary_context_t *context = lv_event_get_user_data(event);
    context->revealed = true;
    vocabulary_update(context, app_state_model(context->app->state), APP_CHANGE_VOCABULARY);
}

static void vocabulary_leave(lv_event_t *event)
{
    vocabulary_context_t *context = lv_event_get_user_data(event);
    ui_app_go_back(context->app);
}

static void vocabulary_speak(lv_event_t *event)
{
    vocabulary_speak_binding_t *binding = lv_event_get_user_data(event);
    vocabulary_context_t *context = binding->context;
    const app_vocabulary_t *deck = &app_state_model(context->app->state)->vocabulary;
    if(!deck->complete && deck->cards && deck->current_index < deck->card_count)
        (void)ui_app_pronounce_word(context->app, deck->cards[deck->current_index].word, binding->accent);
}

static void vocabulary_update(void *user_data, const app_model_t *model, uint32_t changes)
{
    vocabulary_context_t *context = user_data;
    if(!context || !model ||
       (changes != APP_CHANGE_ALL && !(changes & APP_CHANGE_VOCABULARY))) return;
    const app_vocabulary_t *deck = &model->vocabulary;
    char text[128];
    size_t count = deck->card_count;
    size_t index = deck->current_index < count ? deck->current_index : 0;
    const char *current = !deck->complete && count && deck->cards ? deck->cards[index].word : "";
    if(strcmp(context->current_word, current) || context->attempts != deck->reviewed_count) {
        if(context->current_word[0]) ui_app_stop_pronunciation(context->app);
        snprintf(context->current_word, sizeof(context->current_word), "%s", current);
        context->attempts = deck->reviewed_count;
        context->revealed = false;
    }
    unsigned finished = (unsigned)__builtin_popcount(deck->finished_mask);
    snprintf(text, sizeof(text), "%u/%u", finished, (unsigned)count);
    lv_label_set_text(context->progress, text);
    int32_t width = count ? (int32_t)(676 * finished / count) : 0;
    lv_obj_set_width(context->progress_fill, width);
    if(deck->plan.available) {
        unsigned learned = (unsigned)__builtin_popcount(deck->attempted_mask & deck->plan.new_mask);
        unsigned reviewed = (unsigned)__builtin_popcount(deck->attempted_mask & ~deck->plan.new_mask);
        unsigned due = deck->plan.due > reviewed ? deck->plan.due - reviewed : 0;
        unsigned fresh = deck->plan.new_count > learned ? deck->plan.new_count - learned : 0;
        lv_label_set_text_fmt(context->plan, "待复习 %u  ·  今日已学 %u  ·  待学 %u",
                              due, deck->plan.learned_today + learned, fresh);
    } else lv_label_set_text(context->plan, "");
    lv_obj_set_flag(context->answers, LV_OBJ_FLAG_HIDDEN, deck->complete || !context->revealed);
    lv_obj_set_flag(context->reveal, LV_OBJ_FLAG_HIDDEN, deck->complete || context->revealed);
    if(deck->complete) {
        lv_obj_add_flag(context->card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(context->complete, LV_OBJ_FLAG_HIDDEN);
        unsigned known = 0;
        for(size_t i = 0; i < count && i < 32; ++i)
            if(deck->known_mask & (1U << i)) ++known;
        lv_arc_set_value(context->complete_ring, count ? (int32_t)(known * 100 / count) : 0);
        snprintf(text, sizeof(text), "%u", known);
        lv_label_set_text(context->complete_count, text);
        lv_label_set_text(context->complete_heading, count ? "本批完成" : "暂无到期词卡");
        snprintf(text, sizeof(text), "认识 %u · 作答 %u 次", known, deck->reviewed_count);
        if(!count) {
            time_t at = (time_t)deck->plan.next_review_at;
            struct tm local;
            if(at > 0 && localtime_r(&at, &local))
                strftime(text, sizeof(text), "下次复习 %m-%d %H:%M", &local);
            else snprintf(text, sizeof(text), "查看今日计划或稍后再来");
        }
        lv_label_set_text(context->complete_stats, text);
        return;
    }
    lv_obj_remove_flag(context->card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(context->complete, LV_OBJ_FLAG_HIDDEN);
    if(!count || !deck->cards) {
        lv_label_set_text(context->word, "暂无词库");
        lv_label_set_text(context->phonetic, "");
        lv_label_set_text(context->example, "");
        lv_label_set_text(context->translation, "");
        return;
    }
    const app_vocabulary_card_t *word = &deck->cards[index];
    const uint32_t bit = 1U << index;
    lv_label_set_text(context->badge, deck->attempted_mask & bit ? "本轮重测" :
                     !deck->plan.available || deck->plan.new_mask & bit ? "单词回忆" : "到期复习");
    lv_label_set_text(context->word, word->word);
    lv_label_set_text(context->phonetic, word->phonetic ? word->phonetic : "");
    lv_label_set_text(context->meaning, context->revealed ? word->meaning : "先回忆，再看释义");
    lv_obj_set_flag(context->example_box, LV_OBJ_FLAG_HIDDEN,
                    !context->revealed || !word->example || !word->example[0]);
    lv_label_set_text(context->example, word->example ? word->example : "");
    lv_label_set_text(context->translation, word->translation ? word->translation : "");
}

static void vocabulary_destroy(void *user_data)
{
    vocabulary_context_t *context = user_data;
    if(context) ui_app_stop_pronunciation(context->app);
    free(context);
}

static void pronunciation_button(vocabulary_context_t *context, int32_t x,
                                  app_vocabulary_accent_t accent, const char *text)
{
    lv_obj_t *button = ui_ref_button(context->card, x, 35, 78, 38, 19, NULL);
    lv_obj_t *speaker = ui_ref_icon(button, "speaker", 16, UI_REF_INK);
    lv_obj_set_pos(speaker, 11, 11);
    ui_ref_label(button, text, 14, UI_FONT_MEDIUM, UI_REF_INK, 32, 9);
    context->speak[accent] = (vocabulary_speak_binding_t){context, accent};
    lv_obj_add_event_cb(button, vocabulary_speak, LV_EVENT_CLICKED, &context->speak[accent]);
}

static void answer_button(vocabulary_context_t *context, int32_t x, const char *text,
                            const char *icon_name, const char *surface, bool active,
                            lv_event_cb_t callback)
{
    lv_obj_t *button = ui_ref_button(context->answers, x, 0, 218, 65, 33, surface);
    if(active) {
        lv_obj_set_style_bg_color(button, lv_color_hex(UI_REF_CLAY), 0);
        lv_obj_set_style_shadow_color(button, lv_color_hex(UI_REF_CLAY), 0);
        lv_obj_set_style_shadow_opa(button, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(button, 28, 0);
        lv_obj_set_style_shadow_spread(button, -8, 0);
        lv_obj_set_style_shadow_offset_y(button, 12, 0);
    }
    uint32_t color = active ? 0xffffff : UI_REF_INK;
    lv_obj_t *icon = ui_ref_icon(button, icon_name, 18, active ? 0xffffff :
                                 (x == 0 ? UI_REF_FAINT : UI_REF_INK));
    lv_obj_set_pos(icon, x == 229 ? 69 : 76, 24);
    ui_ref_label(button, text, 14, active ? UI_FONT_SEMIBOLD : UI_FONT_MEDIUM,
                  color, x == 229 ? 94 : 101, 22);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, context);
}

ui_screen_t ui_vocabulary_screen_create(ui_app_t *app)
{
    vocabulary_context_t *context = calloc(1, sizeof(*context));
    if(!context) return (ui_screen_t){0};
    context->app = app;
    lv_obj_t *root = ui_ref_shell(app, "背单词", "VOCABULARY");
    lv_obj_t *counter = ui_ref_button(root, 621, 30, 70, 40, 20, "word_counter");
    lv_obj_remove_flag(counter, LV_OBJ_FLAG_CLICKABLE);
    context->progress = ui_centered_label(counter, "", ui_ref_font(13, UI_FONT_SEMIBOLD), UI_REF_INK);
    context->plan = ui_ref_label(root, "", 13, UI_FONT_MEDIUM, UI_REF_MUTED, 237, 45);
    lv_obj_t *progress = ui_box(root, 22, 93, 676, 5, 0xdedede, 3);
    context->progress_fill = ui_box(progress, 0, 0, 0, 5, UI_REF_CLAY, 3);

    context->card = ui_box(root, 22, 116, 676, 499, 0xffffff, 31);
    ui_ref_surface(context->card, "word");
    lv_obj_t *badge = ui_box(context->card, 29, 38, 152, 32, 0xe7e7e7, 16);
    ui_ref_surface(badge, "word_badge");
    lv_obj_t *book = ui_ref_icon(badge, "book", 14, UI_REF_CLAY);
    lv_obj_set_pos(book, 14, 9);
    context->badge = ui_ref_label(badge, "单词回忆", 13, UI_FONT_MEDIUM, UI_REF_MUTED, 35, 6);
    pronunciation_button(context, 483, APP_VOCABULARY_ACCENT_US, "美音");
    pronunciation_button(context, 569, APP_VOCABULARY_ACCENT_UK, "英音");
    context->word = ui_ref_label(context->card, "", 68, UI_FONT_SEMIBOLD, UI_REF_INK, 29, 101);
    lv_obj_set_width(context->word, 618);
    lv_label_set_long_mode(context->word, LV_LABEL_LONG_CLIP);
    lv_obj_add_flag(context->word, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(context->word, vocabulary_speak, LV_EVENT_CLICKED, &context->speak[APP_VOCABULARY_ACCENT_US]);
    context->phonetic = ui_ref_label(context->card, "", 18, UI_FONT_REGULAR, UI_REF_MUTED, 29, 180);
    lv_obj_set_width(context->phonetic, 618);
    lv_label_set_long_mode(context->phonetic, LV_LABEL_LONG_WRAP);
    context->meaning = ui_ref_label(context->card, "", 23, UI_FONT_MEDIUM, UI_REF_INK, 29, 238);
    lv_obj_set_width(context->meaning, 618);
    lv_obj_set_height(context->meaning, 125);
    lv_label_set_long_mode(context->meaning, LV_LABEL_LONG_CLIP);
    lv_obj_t *example = ui_box(context->card, 29, 388, 618, 83, 0xe7e7e7, 23);
    context->example_box = example;
    ui_ref_surface(example, "word_example");
    context->example = ui_ref_label(example, "", 16, UI_FONT_MEDIUM, UI_REF_INK, 18, 18);
    lv_obj_set_width(context->example, 582);
    lv_label_set_long_mode(context->example, LV_LABEL_LONG_WRAP);
    context->translation = ui_ref_label(example, "", 13, UI_FONT_REGULAR, UI_REF_MUTED, 18, 46);
    lv_obj_set_width(context->translation, 582);
    lv_label_set_long_mode(context->translation, LV_LABEL_LONG_WRAP);

    context->answers = ui_box(root, 22, 633, 676, 65, 0, 0);
    lv_obj_set_style_bg_opa(context->answers, LV_OPA_TRANSP, 0);
    answer_button(context, 0, "忘记", "x", "word_answer", false, vocabulary_unknown);
    answer_button(context, 229, "模糊", "undo", "word_again", false, vocabulary_fuzzy);
    answer_button(context, 458, "认识", "check", NULL, true, vocabulary_known);
    context->reveal = ui_button(root, 22, 633, 676, 65, UI_REF_INK, 33);
    ui_centered_label(context->reveal, "查看释义", ui_ref_font(18, UI_FONT_SEMIBOLD), 0xffffff);
    lv_obj_add_event_cb(context->reveal, vocabulary_reveal, LV_EVENT_CLICKED, context);

    context->complete = ui_box(root, 22, 116, 676, 586, 0xffffff, 31);
    ui_ref_surface(context->complete, "word_complete");
    context->complete_ring = ui_ref_ring(context->complete, 271, 153, 135, 11);
    context->complete_count = ui_ref_label(context->complete, "", 32, UI_FONT_SEMIBOLD, UI_REF_INK, 271, 200);
    lv_obj_set_width(context->complete_count, 135);
    lv_obj_set_style_text_align(context->complete_count, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *heading = ui_ref_label(context->complete, "本批完成", 22, UI_FONT_SEMIBOLD, UI_REF_INK, 0, 307);
    context->complete_heading = heading;
    lv_obj_set_width(heading, 676);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    context->complete_stats = ui_ref_label(context->complete, "", 13, UI_FONT_REGULAR, UI_REF_MUTED, 0, 351);
    lv_obj_set_width(context->complete_stats, 676);
    lv_obj_set_style_text_align(context->complete_stats, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *reset = ui_button(context->complete, 281, 392, 114, 50, UI_REF_INK, 25);
    ui_centered_label(reset, "返回", ui_ref_font(14, UI_FONT_MEDIUM), 0xffffff);
    lv_obj_add_event_cb(reset, vocabulary_leave, LV_EVENT_CLICKED, context);
    vocabulary_update(context, app_state_model(app->state), APP_CHANGE_ALL);
    return (ui_screen_t){root, context, vocabulary_update, vocabulary_destroy, NULL};
}
