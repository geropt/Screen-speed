#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _objects_t {
    lv_obj_t *main;
    lv_obj_t *speed_text_container;
    lv_obj_t *current_speed_label;
    lv_obj_t *speed_unit_label;
    lv_obj_t *speed_limit_container;
    lv_obj_t *speed_limit_label;
    lv_obj_t *speed_limit_warning_label;
    lv_obj_t *street_name;
    lv_obj_t *overspeed_ring;
} objects_t;

extern objects_t objects;

enum ScreensEnum {
    SCREEN_ID_MAIN = 1,
};

void create_screen_main();
void tick_screen_main();

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();


#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/