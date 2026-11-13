/* Сгенерировано tools/gen_nav_images.py — не править вручную */
#ifndef HUD_NAV_IMAGES_H
#define HUD_NAV_IMAGES_H
#include "lvgl.h"

#define NAV_AREA_W 192
#define NAV_AREA_H 130
#define NAV_SMALL_W 38
#define NAV_SMALL_H 23

typedef struct { const lv_img_dsc_t *img; int16_t x, y; } NavImg;   /* x,y — смещение картинки в своей области */

/* Набор стрелок: turn[16] — сектор 0..15 (22.5 град, против часовой от 'прямо'),
   round[16] — кольцо, сектор съезда; exit/fork/uturn: [0] налево, [1] направо */
typedef struct {
    const NavImg *turn, *round, *exit, *fork, *uturn, *arrived;
} NavSet;

extern const NavSet nav_big;     /* основная стрелка: последний кадр анимации (перспектива), область NAV_AREA_W x NAV_AREA_H, индексные картинки */
extern const NavSet nav_small;   /* следующий манёвр: маска, область NAV_SMALL_W x NAV_SMALL_H, цвет — img_recolor */

#endif
