#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Screens

enum ScreensEnum {
    _SCREEN_ID_FIRST = 1,
    SCREEN_ID_MAIN = 1,
    _SCREEN_ID_LAST = 1
};

typedef struct _objects_t {
    lv_obj_t *main;
    lv_obj_t *obj0;
    lv_obj_t *obj0__arc_indicator_left;
    lv_obj_t *obj0__arc_indicator_right;
    lv_obj_t *speed_text_container;
    lv_obj_t *current_speed_label;
    lv_obj_t *speed_unit_label;
    lv_obj_t *speed_limit_container;
    lv_obj_t *speed_limit_label;
    lv_obj_t *speed_limit_warning_label;
    lv_obj_t *street_name;
} objects_t;

extern objects_t objects;

void create_screen_main();
void tick_screen_main();

void create_user_widget_arc_indicator(lv_obj_t *parent_obj, void *flowState, int startWidgetIndex);
void tick_user_widget_arc_indicator(void *flowState, int startWidgetIndex);

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/