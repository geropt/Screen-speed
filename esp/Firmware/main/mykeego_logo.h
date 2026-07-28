#ifndef MYKEEGO_LOGO_H
#define MYKEEGO_LOGO_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Logo de mykeego como mascara alfa (LV_IMG_CF_ALPHA_8BIT), 300x144.
// El color se aplica por recolor en el widget que lo muestra.
extern const lv_img_dsc_t mykeego_logo;

#ifdef __cplusplus
}
#endif

#endif // MYKEEGO_LOGO_H
