/*
  hud_mockup.c — экран HUD на живых данных (v7)
  ------------------------------------------------------------------------------
  v7:
    - Раскладка без изменений (v6), но все элементы получили хэндлы и
      обновляются из HudData (CAN через BLE-сниффер).
    - Обновление перенесено в lv_timer (50 мс): он выполняется внутри
      lv_timer_handler() под мьютексом LVGL. Прежний update_turn_signals()
      из loop() трогал LVGL с другого ядра без мьютекса — убран.
    - Мигалки: зелёные для поворотников, КРАСНЫЕ при аварийке (BM_Warnblinken).
    - Поле без свежих данных (бит valid) скрывается.
    - Знак: vze_table.h -> ACC_Tempolimit -> сырой код мелким серым.
      Превышение (VZE_Warnung) — фон знака мигает красным.
    - LKA (LDW_02 0x397): зелёный активен, жёлтый включён/не держит,
      оранжевый предупреждение о выходе за линию.
    - Стрелка: пока текстом (MainElement / направление / дистанция).
*/

#include "lvgl.h"
#include "esp_system.h"
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "hud_config.h"

/* Проверка при сборке: нужен lv_conf.h из config/ (Documents/Arduino/libraries/lv_conf.h), а не старый с пулом 48 КБ */
#ifdef ESP_PLATFORM
#if LV_MEM_CUSTOM || (LV_MEM_SIZE < (96U * 1024U))
#error "lv_conf.h: LV_MEM_SIZE < 96 КБ — скопируйте config/lv_conf.h из репозитория в Documents/Arduino/libraries (рядом с папкой lvgl)"
#endif
#endif
#include "hud_mockup.h"
#include "hud_data.h"
#include "vze_table.h"
#include "hud_images.h"
#include "hud_nav_images.h"
#include "hud_anim.h"
#include "hud_sound.h"
#include "hud_light.h"
#include "hud_ota.h"
#include "hud_source.h"
#include "hud_sounds.h"
#include "psd_speedlimit.h"
#include "hud_log.h"
#include "cam_alert.h"
#include "hud_diag.h"
#include "hud_cams.h"
#include "hud_min_images.h"
#include "src/lcd_bl_bsp/lcd_bl_pwm_bsp.h"
LV_FONT_DECLARE(hud_font_splash);      /* заставка «HUD VAG» (Inter Display Medium 72 px, только нужные буквы) */
LV_FONT_DECLARE(hud_font_splash_sub);  /* подпись платформы под ней (22 px) */

/* Шрифты: Montserrat Bold (SIL OFL 1.1) с табличными цифрами — см. tools/fonts/ */
LV_FONT_DECLARE(hud_font_speed);  /* скорость, цифры ~84 px                          */
LV_FONT_DECLARE(hud_font_gear);   /* режим КП, ~15 px: P R N D S M E Offroad 1-9     */
LV_FONT_DECLARE(hud_font_route);  /* маршрут, дистанция, ~19 px: 0-9 : . km h mi ft км м ч */
LV_FONT_DECLARE(hud_font_small);  /* скорость ACC / лимитера, знак 2 цифры, ~18 px   */
LV_FONT_DECLARE(hud_font_sign2);  /* знак 2 цифры, Roboto Condensed Bold, ~28 px     */
LV_FONT_DECLARE(hud_font_sign3);  /* знак 3 цифры, Roboto Condensed Bold, ~21 px     */
LV_FONT_DECLARE(hud_font_kmh);
LV_FONT_DECLARE(hud_font_menu);
LV_FONT_DECLARE(hud_font_fuel);   /* «сколько заправить»: 0-9 . L л g a l          */   /* меню настроек: латиница + кириллица, 20 px      */    /* подпись км/ч / km/h / mph, ~29 px               */

#ifdef __cplusplus
extern "C" {
#endif
    void arduino_printf(const char *format, ...);
#ifdef __cplusplus
}
#endif

/* ---------- цвета ---------- */
#define C_GREEN      0x4ad94a
#define C_BLINK_GRN  0x00cc00
#define C_BLINK_RED  0xe00000
#define C_GRAY       0x888888
#define C_LABEL_GRAY 0x8c8c8c
#define C_CAR_BODY   0xe6e6e6
#define C_CAR_OPEN   0xeb1c1c
#define C_CAR_LIGHTS 0xc81414
#define C_ORANGE     0xffa500
#define C_YELLOW     0xffcc00
#define C_OBJ_WARN   0xe01414   /* машина впереди при опасной дистанции */
#define C_FUEL       0x505050   /* полоска расхода  */
#define C_OVERSPEED  0xff1a1a   /* обводка скорости при превышении */
#define C_SWA        0xff2020   /* Side Assist                     */
#define C_SIGN_RED   0xcc0000
#define SIGN_D       64    /* диаметр знака         */
#define SIGN_RING    7     /* толщина красного кольца */
#define C_SIGN_WARN  0xff8080
#define C_NAV_ARROW  0x30b0f0   /* маршрут — голубой, как у Audi */
#define C_NAV_NEXT   0xa8a8a8
#define TOP_BAR_W    192         /* HUD_NAV_BAR_TOP: ширина полоски = ширина области стрелки */
#define TOP_BAR_H    4
#if HUD_NAV_BAR_TOP
#define NAV_DY       6           /* кадр стрелки 192x130: одинаковые отступы между полоской бара и строкой дистанции */
#else
#define NAV_DY       0
#endif
#define ANIM_DY      NAV_DY      /* кадр анимации 192x130 стоит там же, где статичный */
#define NAV_BOTTOM_Y 165         /* низ строки «дистанция + малая стрелка» в области навигации (от её верха), px */
#define C_LINK_BLE   0x1f4fb0   /* BLE подключён */
#define C_LINK_CAN   0x1e8a3c   /* данные по CAN */
#define C_BAR_ON     0x4a90d9
#define C_BAR_OFF    0x222222

/* ---------- хэндлы ---------- */
static lv_obj_t *sign_obj, *sign_lbl, *sign_end_line, *sign_car_l, *sign_car_r;
static lv_obj_t *spd_outline[8];          /* красная обводка цифр скорости при превышении */
static lv_obj_t *assist_box, *as_own, *as_front, *as_arcs, *as_line_l, *as_line_r;
static lv_obj_t *jam_icon, *lim_icon;
static lv_obj_t *ps_box, *ps_tri;           /* предупреждение pre sense */

static lv_obj_t *fuel_bar;               /* расход топлива, правый край */
static lv_obj_t *arrow_ph, *anim_img, *arrow_img, *arrow_lbl, *nav_dist_lbl, *next_img;
static lv_obj_t *bar_seg[16];
static lv_obj_t *top_bar, *top_fill;      /* HUD_NAV_BAR_TOP: тонкая полоска прогресса над стрелкой */
static lv_obj_t *route_icon, *route_lbl;
static lv_obj_t *speed_lbl;
static lv_obj_t *acc_dot, *acc_speed_lbl;   /* acc_dot — значок установки скорости ACC */
static lv_obj_t *turn_left_container, *turn_right_container;
static lv_obj_t *turn_left_sym, *turn_right_sym;
static lv_obj_t *gear_lbl, *kmh_lbl, *tank_icon, *tank_lbl;
static lv_obj_t *car_box, *car_part[6], *car_under[6];   /* FL FR RL RR HOOD TRUNK — как биты DOOR_* */
static lv_obj_t *full_root;               /* контейнер полного режима: скрывается целиком в минималистичном */
static lv_obj_t *link_lbl, *pair_lbl;
static bool link_pf;                    /* сопряжение не принято — значка источника нет */
static bool tsr_shown;                  /* кружок TSR показан — значок источника на его месте не рисуем */

/* ---------- мелкие помощники ---------- */
static void vis(lv_obj_t *o, bool v)
{
    bool hidden = lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN);
    if (v && hidden)        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else if (!v && !hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void set_text(lv_obj_t *l, const char *s)
{
    const char *cur = lv_label_get_text(l);
    if (!cur || strcmp(cur, s) != 0) lv_label_set_text(l, s);
}

static void set_text_color(lv_obj_t *o, uint32_t hex)
{
    lv_color_t c = lv_color_hex(hex);
    if (lv_obj_get_style_text_color(o, LV_PART_MAIN).full != c.full)
        lv_obj_set_style_text_color(o, c, 0);
}

static void set_bg_color(lv_obj_t *o, uint32_t hex)
{
    lv_color_t c = lv_color_hex(hex);
    if (lv_obj_get_style_bg_color(o, LV_PART_MAIN).full != c.full)
        lv_obj_set_style_bg_color(o, c, 0);
}

static void set_border_color(lv_obj_t *o, uint32_t hex)
{
    lv_color_t c = lv_color_hex(hex);
    if (lv_obj_get_style_border_color(o, LV_PART_MAIN).full != c.full)
        lv_obj_set_style_border_color(o, c, 0);
}

static void set_font(lv_obj_t *o, const lv_font_t *f)
{
    if (lv_obj_get_style_text_font(o, LV_PART_MAIN) != f)
        lv_obj_set_style_text_font(o, f, 0);
}

static void set_img_color(lv_obj_t *o, uint32_t hex)
{
    /* Для масок ALPHA_8BIT цвет картинки = img_recolor, НО LVGL 8 берёт его,
       только если img_recolor_opa > 0 (иначе маска рисуется чёрной).
       Поэтому у всех таких картинок recolor_opa = COVER. */
    lv_color_t c = lv_color_hex(hex);
    if (lv_obj_get_style_img_recolor(o, LV_PART_MAIN).full != c.full)
        lv_obj_set_style_img_recolor(o, c, 0);
}

static const char *gear_text(uint8_t g)
{
    switch (g) {
    case 1: return "P"; case 2: return "R"; case 3: return "N"; case 4: return "D";
    case 5: return "S"; case 6: return "M"; case 8: return "E"; case 12: return "Offroad";
    default: return NULL;
    }
}

enum { SIGN_HIDE, SIGN_LIMIT, SIGN_END, SIGN_RAW, SIGN_NOOVT };

typedef struct { uint8_t mode; uint16_t val; uint8_t src; } SignItem;   /* src: 0 —, 1 VZE, 2 PSD */

/* Код VZE -> знак. allow_fallback: для основного знака можно взять ACC_Tempolimit
   и показать сырой код; для дополнительных знаков 2/3 — только то, что точно известно. */
static SignItem vze_eval(uint8_t code, bool suppress, uint8_t tl, bool allow_fallback)
{
    SignItem it = { SIGN_HIDE, 0, 1 };
    if (!code || suppress) return it;
    const VzeEntry *e = &vze_table[code];
    if (e->type == VZE_LIMIT)               { it.mode = SIGN_LIMIT; it.val = e->value; }
    else if (e->type == VZE_END_LIMIT)      { it.mode = SIGN_END; }
    else if (e->type == VZE_NO_OVERTAKE)    { it.mode = SIGN_NOOVT; }
    else if (e->type != VZE_UNKNOWN)        { it.mode = SIGN_HIDE; }
    else if (vze_linear_kmh(code))          { it.mode = SIGN_LIMIT; it.val = vze_linear_kmh(code); }
    else if (!allow_fallback)               { it.mode = SIGN_HIDE; }
    else if (tl == ACC_TEMPOLIMIT_END)      { it.mode = SIGN_END; }
    else if (acc_tempolimit_kmh(tl))        { it.mode = SIGN_LIMIT; it.val = acc_tempolimit_kmh(tl); }
    else                                    { it.mode = SIGN_RAW; it.val = code; }
    return it;
}

/* ---------- камеры фиксации скорости (HUD_CAMS), на месте диагностики под знаком ---------- */
static bool g_cam_active;                 /* камера сейчас показана: диагностика уступает место */
static bool    g_cam_hit;                 /* то же + данные для минималистичного режима (заполняет cams_update) */
static uint8_t g_cam_lim;
static bool    g_cam_over;
static char    g_cam_txt[16];
static volatile uint8_t g_units, g_tol;   /* определены ниже (tentative definition) */
static volatile bool g_cams_on, g_cam_beep;
#if HUD_CAMS
static lv_obj_t *car_img(lv_obj_t *parent, const lv_img_dsc_t *src, int x, int y, uint32_t col);
static volatile uint8_t g_lang;   /* определён ниже */
static lv_obj_t *cm_sq, *cm_disc, *cm_lim, *cm_dist, *cm_eye;

/* Знак камеры — на месте обычного знака, того же размера (64 px), самый приоритетный:
   круглый знак ограничения (Ø52, кольцо 6) на жёлтом квадрате, под ним значок камеры со вспышкой и расстояние. */
static void cams_build(lv_obj_t *scr)
{
    cm_sq = lv_obj_create(scr);
    lv_obj_set_size(cm_sq, SIGN_D, SIGN_D); lv_obj_set_pos(cm_sq, 1, 2);
    lv_obj_set_style_radius(cm_sq, 6, 0);
    lv_obj_set_style_bg_color(cm_sq, lv_color_hex(C_YELLOW), 0);
    lv_obj_set_style_border_width(cm_sq, 0, 0);
    lv_obj_set_style_pad_all(cm_sq, 0, 0);
    lv_obj_clear_flag(cm_sq, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    cm_disc = lv_obj_create(cm_sq);                     /* круглый знак ограничения камеры */
    lv_obj_set_size(cm_disc, 52, 52); lv_obj_set_pos(cm_disc, 6, 6);
    lv_obj_set_style_radius(cm_disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cm_disc, lv_color_white(), 0);
    lv_obj_set_style_border_color(cm_disc, lv_color_hex(C_SIGN_RED), 0);
    lv_obj_set_style_border_width(cm_disc, 6, 0);
    lv_obj_set_style_pad_all(cm_disc, 0, 0);
    lv_obj_clear_flag(cm_disc, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    cm_lim = lv_label_create(cm_disc);  lv_label_set_text(cm_lim, "");
    lv_obj_set_style_text_font(cm_lim, &hud_font_sign2, 0);
    lv_obj_set_style_text_color(cm_lim, lv_color_black(), 0);
    lv_obj_center(cm_lim);

    cm_eye = car_img(scr, &img_cam_icon, 5, 80, C_YELLOW);   /* значок камеры со вспышкой */

    cm_dist = lv_label_create(scr);  lv_label_set_text(cm_dist, "");
    lv_obj_set_style_text_font(cm_dist, &hud_font_route, 0);
    lv_obj_set_style_text_color(cm_dist, lv_color_hex(C_YELLOW), 0);
    lv_label_set_long_mode(cm_dist, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(cm_dist, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(cm_dist, 130); lv_obj_set_pos(cm_dist, 50, 86);   /* справа от значка камеры */

    lv_obj_add_flag(cm_sq, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(cm_dist, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(cm_eye, LV_OBJ_FLAG_HIDDEN);
}

/* true — камера впереди показана. *lim_for_speed уменьшается до лимита камеры вблизи неё. */
static bool cams_update(uint32_t now, const HudData *d, uint16_t *lim_for_speed)
{
    static uint32_t beeped = 0xFFFFFFFFu;
    cam_hit_t h; bool hit = false;
    g_diag.ticks++;
    if (d->valid & V_SPEED) { if (d->speed_kmh > g_diag.max_spd) g_diag.max_spd = d->speed_kmh; }
    bool gate = false;
    if (!g_cams_on)                          g_diag.no_cams_on++;
    else if (!hud_cams_ready())              g_diag.no_db++;
    else if (!(d->valid & V_GPS))            g_diag.no_gps++;
    else if (!(d->valid & V_HDG))            g_diag.no_hdg++;
    else if (!(d->valid & V_SPEED))          g_diag.no_spd++;
    else if (d->gps_fix != 3)                g_diag.no_fix++;
    else if (d->gps_sat < 4)                 g_diag.no_sat++;
    else if (d->gps_hdop_x10 > HUD_CAM_MAX_HDOP_X10) g_diag.no_hdop++;
    else if (d->gps_hdg_x10 == 0xFFFF)       g_diag.no_hdg_val++;
    else if (d->speed_kmh < HUD_CAM_MIN_SPEED_KMH)   g_diag.too_slow++;
    else gate = true;
    if (gate) {
        g_diag.q_run++; g_diag.lat_e6 = d->gps_lat_e6; g_diag.lon_e6 = d->gps_lon_e6;
        hit = cam_query(now, d->gps_lat_e6 / 1e6, d->gps_lon_e6 / 1e6, d->gps_hdg_x10 / 10.0f, (float)d->speed_kmh, &h);
        if (hit) g_diag.q_hit++;
    }
    /* Условия допуска на миг не выполнились (курс/HDOP/скорость/GNSS мигнули) — камеру не гасим сразу,
       иначе на её месте на мгновение вспыхивает обычный знак (VZE/PSD). Если запрос шёл и камеры нет — это настоящий конец. */
    static cam_hit_t s_h_last; static uint32_t s_t_last = 0; static bool s_was = false;
    if (hit)                      { s_h_last = h; s_t_last = now; s_was = true; }
    else if (!gate && g_cams_on && s_was && (now - s_t_last) <= 1500) {
        float left = s_h_last.dist_m - d->speed_kmh / 3.6f * (now - s_t_last) / 1000.0f;   /* расстояние продолжает уменьшаться */
        if (left > 0) { h = s_h_last; h.dist_m = left; hit = true; } else s_was = false;
    }
    else                          s_was = false;

    g_cam_active = hit;
    g_cam_hit = hit;
    vis(cm_sq, hit); vis(cm_dist, hit); vis(cm_eye, hit);
    if (hit) vis(sign_obj, false);                      /* знак камеры приоритетнее обычного знака */
    if (!hit) return false;

    char b[16];
    if (g_units == HUD_UNITS_MI) snprintf(b, sizeof b, "%dft", (int)(h.dist_m * 3.281f / 50) * 50);
    else                         snprintf(b, sizeof b, "%d%s",  (int)(h.dist_m / 10) * 10, g_lang == HUD_LANG_EN ? "m" : "м");   /* без пробела: 330м / 330m */
    set_text(cm_dist, b);
    snprintf(g_cam_txt, sizeof g_cam_txt, "%s", b);
    g_cam_lim = h.limit;
    if (h.limit) snprintf(b, sizeof b, "%u", h.limit); else snprintf(b, sizeof b, "?");
    set_font(cm_lim, h.limit >= 100 ? &hud_font_sign3 : &hud_font_sign2);
    set_text(cm_lim, b); lv_obj_center(cm_lim);

    bool over = h.limit && d->speed_kmh > h.limit + g_tol;
    g_cam_over = over;
    bool ph   = over && (now % 400) < 200;
    set_bg_color(cm_disc, ph ? C_SIGN_WARN : 0xffffff);
    lv_obj_set_style_img_recolor(cm_eye, lv_color_hex(over ? 0xff3030 : C_YELLOW), 0);

    if (h.idx != beeped) {                              /* новая камера */
        beeped = h.idx;
        g_diag.shown++; g_diag.last_idx = h.idx; g_diag.last_lim = h.limit; g_diag.last_dist_m = (uint16_t)h.dist_m; g_diag.last_spd = d->speed_kmh;
        arduino_printf("[cam] #%lu лимит %u с %.0f м, v=%u, курс %u/кам %d\n", (unsigned long)h.idx, h.limit,
                       h.dist_m, d->speed_kmh, d->gps_hdg_x10 / 10, (int)h.dir_deg);
        if (g_cam_beep && h.limit && d->speed_kmh > h.limit) hud_sound_play(hud_snd_swa_beep, hud_snd_swa_beep_len);
    }
    if (h.limit && h.dist_m <= HUD_CAM_OVERSPEED_M && (*lim_for_speed == 0 || h.limit < *lim_for_speed))
        *lim_for_speed = h.limit;
    return true;
}
#endif

static int tsr_text_w(lv_obj_t *l, const lv_font_t *f)
{
    lv_point_t p = {0, 0};
    lv_txt_get_size(&p, lv_label_get_text(l), f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return p.x;
}

/* середина свободного места верхней строки между «сколько заправить» и передачей (зона 376..626) */
static int top_gap_cx(void)
{
    int left = 376, right = 626;
    if (!lv_obj_has_flag(tank_lbl, LV_OBJ_FLAG_HIDDEN)) left = 396 + tsr_text_w(tank_lbl, &hud_font_fuel);
    if (!lv_obj_has_flag(gear_lbl, LV_OBJ_FLAG_HIDDEN)) right = 626 - tsr_text_w(gear_lbl, &hud_font_gear);
    return (left + right) / 2;
}

#if HUD_TSR_STATE
#define C_TSR_YELLOW 0xffcc00
#define C_TSR_GREEN  0x00c800
#define C_TSR_RED    0xe60000
#define TSR_D        10                  /* диаметр кружка, px */
static lv_obj_t *tsr_dot;               /* заполненный кружок в верхней строке: между «сколько заправить» и передачей */

static void tsr_build(lv_obj_t *scr)
{
    tsr_dot = lv_obj_create(scr);
    lv_obj_remove_style_all(tsr_dot);
    lv_obj_set_size(tsr_dot, TSR_D, TSR_D);
    lv_obj_set_pos(tsr_dot, 520 - TSR_D / 2, 11 - TSR_D / 2);
    lv_obj_set_style_radius(tsr_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(tsr_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(tsr_dot, lv_color_hex(C_TSR_YELLOW), 0);
    lv_obj_clear_flag(tsr_dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(tsr_dot, LV_OBJ_FLAG_HIDDEN);
}

static void tsr_update(const HudData *d, bool vze_on)
{
    bool show = vze_on && (d->valid & V_SIGN) && (d->tsr_hint == 0 || d->tsr_hint == 3 || d->tsr_hint == 5);
    if (show) {
        uint32_t col = d->tsr_hint == 3 ? C_TSR_GREEN : d->tsr_hint == 5 ? C_TSR_RED : C_TSR_YELLOW;
        set_bg_color(tsr_dot, col);
        /* по центру между правым краем «сколько заправить» и левым краем передачи (если их нет — края зоны 376..626) */
        int cx = top_gap_cx();
        if (lv_obj_get_x(tsr_dot) != cx - TSR_D / 2) lv_obj_set_x(tsr_dot, cx - TSR_D / 2);
    }
    tsr_shown = show;
    vis(tsr_dot, show);
}
#endif

#if HUD_PSD_DIAG
/* ---------- диагностика привязки к карте (HUD_PSD_DIAG), под знаком ---------- */
#define DG_OFF 0x444444
static lv_obj_t *dg_dot, *dg_cc, *dg_src, *dg_bar[3], *dg_home, *dg_gps;


static void diag_build(lv_obj_t *scr)
{
    dg_dot = lv_obj_create(scr);
    lv_obj_set_size(dg_dot, 12, 12); lv_obj_set_pos(dg_dot, 3, 72);
    lv_obj_set_style_radius(dg_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dg_dot, 0, 0);
    lv_obj_set_style_bg_color(dg_dot, lv_color_hex(C_GRAY), 0);
    lv_obj_clear_flag(dg_dot, LV_OBJ_FLAG_SCROLLABLE);

    dg_cc = lv_label_create(scr);  lv_label_set_text(dg_cc, "");
    lv_obj_set_style_text_font(dg_cc, LV_FONT_DEFAULT, 0); lv_obj_set_pos(dg_cc, 20, 70);
    dg_src = lv_label_create(scr); lv_label_set_text(dg_src, "");
    lv_obj_set_style_text_font(dg_src, LV_FONT_DEFAULT, 0); lv_obj_set_pos(dg_src, 46, 70);

    for (int i = 0; i < 3; i++) {                      /* полоски 5/9/13 px, общий низ y=106 */
        int h = 5 + 4 * i;
        dg_bar[i] = lv_obj_create(scr);
        lv_obj_set_size(dg_bar[i], 5, h); lv_obj_set_pos(dg_bar[i], 3 + 7 * i, 106 - h);
        lv_obj_set_style_radius(dg_bar[i], 0, 0);
        lv_obj_set_style_border_width(dg_bar[i], 0, 0);
        lv_obj_set_style_bg_color(dg_bar[i], lv_color_hex(DG_OFF), 0);
        lv_obj_clear_flag(dg_bar[i], LV_OBJ_FLAG_SCROLLABLE);
    }
    dg_home = lv_label_create(scr); lv_label_set_text(dg_home, LV_SYMBOL_HOME);
    lv_obj_set_style_text_font(dg_home, LV_FONT_DEFAULT, 0); lv_obj_set_pos(dg_home, 28, 92);
    dg_gps = lv_label_create(scr);  lv_label_set_text(dg_gps, LV_SYMBOL_GPS);
    lv_obj_set_style_text_font(dg_gps, LV_FONT_DEFAULT, 0); lv_obj_set_pos(dg_gps, 46, 92);
}

static void diag_update(uint32_t now, uint8_t src)
{
    psd_diag_t g; psd_get_diag(now, &g);
    char b[96];
    {   /* камера впереди — место занято ею */
        bool show = !g_cam_active;
        vis(dg_dot, show); vis(dg_cc, show); vis(dg_src, show); vis(dg_home, show); vis(dg_gps, show);
        for (int i = 0; i < 3; i++) vis(dg_bar[i], show);
    }

    /* точка: серая — PSD_05 молчит; зелёная — не Off-Road, категория сегмента != 7 (Init)
       и ADAS-качество сегмента 1 */
    if (!g.pos_fresh) set_bg_color(dg_dot, C_GRAY);
    else set_bg_color(dg_dot, (g.pos_err != 7 && g.cat_cur != 7 && g.adas_q) ? C_GREEN : 0xff3030);

    if (!g.m0_fresh) set_text(dg_cc, "--");
    else if (g.country == 178) set_text(dg_cc, "RU");
    else if (g.country == 20)  set_text(dg_cc, "BY");
    else { snprintf(b, sizeof b, "%u", g.country); set_text(dg_cc, b); }

    /* V — знак от VZE_01; для PSD вторая буква — источник знака в карте:
       A — VZA (ассистент), O — VZO (онлайн), B — оба */
    if (src == 2) {
        char t[3] = { 'P', g.sign_src_ok ? (g.sign_src == 2 ? 'O' : g.sign_src == 3 ? 'B' : 'A') : 0, 0 };
        set_text(dg_src, t);
    } else set_text(dg_src, src == 1 ? "V" : "");
    set_text_color(dg_src, src == 1 ? C_YELLOW : C_GREEN);

    int lvl = g.m0_fresh && g.qual_avail ? (g.qual_tempo + 1) / 2 : 0;
    for (int i = 0; i < 3; i++) set_bg_color(dg_bar[i], i < lvl ? C_GREEN : DG_OFF);

    set_text_color(dg_home, g.pos_fresh && g.urban_cur ? C_YELLOW : DG_OFF);
    set_text_color(dg_gps,  g.m0_fresh && g.guidance ? C_GREEN : DG_OFF);

    static uint32_t log_t = 0;
    if (now - log_t >= 10000u) {
        log_t = now;
        snprintf(b, sizeof b, "PSD diag n04=%lu q1=%lu c7=%lu n05=%lu off=%lu",
                 (unsigned long)g.n04, (unsigned long)g.n04_q1, (unsigned long)g.n04_cat7,
                 (unsigned long)g.n05, (unsigned long)g.n05_off);
        char l[256];
        snprintf(l, sizeof l, "%s | seg cat=%u adas=%u urb=%u pos=%u err=%u | mm=%u geo=%u cc=%u g=%u qa=%u qt=%u src=%u\n", b,
                 g.cat_cur, g.adas_q, g.urban_cur, g.pos_fresh, g.pos_err, g.mm_qual, g.geom_qual,
                 g.country, g.guidance, g.qual_avail, g.qual_tempo, g.sign_src);
        hud_log_write(l);
    }
}
#endif

/* Дистанция BAP (×10 + единица) -> текст. Шрифт hud_font_route содержит
   только символы 0x20..0x6D, поэтому из единиц доступны "m", "km", "mi". */
/* ---------- язык и единицы ---------- */
static volatile uint8_t g_lang  = HUD_LANG;
static volatile uint8_t g_units = HUD_UNITS;

void    hud_set_lang(uint8_t lang)   { g_lang  = lang  == HUD_LANG_EN  ? HUD_LANG_EN  : HUD_LANG_RU;  }
void    hud_set_units(uint8_t units) { g_units = units == HUD_UNITS_MI ? HUD_UNITS_MI : HUD_UNITS_KM; }
uint8_t hud_get_lang(void)           { return g_lang;  }
uint8_t hud_get_units(void)          { return g_units; }
static volatile bool    g_psd   = HUD_PSD_LIMITS;
static volatile bool    g_anim_on = HUD_ANIM_ON;          /* анимированные стрелки: показ (нужен /arrows.bin) */
static volatile uint8_t g_mode  = HUD_DISPLAY_MODE;       /* режим отображения: HUD_MODE_FULL / HUD_MODE_MIN */
static volatile bool    g_vze   = HUD_VZE_SIGNS;
static volatile uint8_t g_fmode = HUD_FUEL_AVG ? 1 : 0;   /* полоска расхода: 0 мгновенный, 1 средний, 2 выкл */
static volatile bool    g_gal   = HUD_VOLUME_GAL;
void    hud_set_gallons(bool on)     { g_gal = on; }
bool    hud_get_gallons(void)        { return g_gal; }
void    hud_set_vze(bool on)         { g_vze = on; }
bool    hud_get_vze(void)            { return g_vze; }
void    hud_set_fuel_avg(bool on)    { g_fmode = on ? 1 : 0; }
bool    hud_get_fuel_avg(void)       { return g_fmode == 1; }
void    hud_set_fuel_mode(uint8_t m) { g_fmode = m > 2 ? 2 : m; }
uint8_t hud_get_fuel_mode(void)      { return g_fmode; }
static volatile uint8_t g_tank  = HUD_TANK_L;             /* объём бака, л (меню 30..90) */
void    hud_set_tank(uint8_t l)      { g_tank = l < 30 ? 30 : l > 90 ? 90 : l; }
uint8_t hud_get_tank(void)           { return g_tank; }
static volatile bool    g_cams_on  = HUD_CAMS_ON;         /* камеры: показ */
static volatile bool    g_cam_beep = HUD_CAM_BEEP;        /* камеры: сигнал */
void    hud_set_cams(bool on)        { g_cams_on = on; }
bool    hud_get_cams(void)           { return g_cams_on; }
void    hud_set_cam_beep(bool on)    { g_cam_beep = on; }
bool    hud_get_cam_beep(void)       { return g_cam_beep; }
void    hud_set_psd(bool on)         { g_psd = on; }
bool    hud_get_psd(void)            { return g_psd; }
void    hud_set_anim(bool on)        { g_anim_on = on; }
bool    hud_get_anim(void)           { return g_anim_on; }
void    hud_set_mode(uint8_t mode)   { g_mode = mode == HUD_MODE_MIN ? HUD_MODE_MIN : HUD_MODE_FULL; }
uint8_t hud_get_mode(void)           { return g_mode; }
static volatile uint8_t g_tol   = HUD_OVERSPEED_TOL_KMH;   /* допуск превышения, км/ч (меню 0..20) */
void    hud_set_overspeed_tol(uint8_t kmh) { g_tol = kmh > 20 ? 20 : kmh; }
uint8_t hud_get_overspeed_tol(void)  { return g_tol; }

typedef struct { const char *kmh, *m, *km, *h; } HudTxt;
static const HudTxt TXT_RU = { "км/ч", "м", "км", "ч" };
static const HudTxt TXT_EN = { "km/h", "m", "km", "h" };
static inline const HudTxt *txt(void) { return g_lang == HUD_LANG_EN ? &TXT_EN : &TXT_RU; }

/* скорость для показа: км/ч -> mph в режиме миль. Знаки не пересчитываются:
   на знаке — то, что написано на дорожном знаке */
static inline unsigned spd(unsigned kmh)
{
    return g_units == HUD_UNITS_MI ? (kmh * 1000u + 804u) / 1609u : kmh;
}

/* Дистанция BAP (x10 + единица MIB: 0 м, 1 км, 2 ярд, 3 фут, 4 миля, 5 1/4 мили) -> текст.
   В режиме миль метрика переводится: до 0.1 мили — футы (шаг 10), дальше — мили.
   Имперские единицы (если так настроен MMI) показываются как есть, ярды — в футах.
   Для имперских единиц сокращения латиницей на обоих языках (mi, ft). */
static void fmt_dist(char *b, size_t n, uint32_t x10, uint8_t unit)
{
    const HudTxt *t = txt();
    if (g_units == HUD_UNITS_MI && (unit == 0 || unit == 1)) {
        uint32_t m = (unit == 0) ? x10 / 10 : x10 * 100;            /* метры */
        if (m < 161) {
            uint32_t ft = ((m * 3281u + 500u) / 1000u + 5u) / 10u * 10u;
            snprintf(b, n, "%luft", (unsigned long)ft);
            return;
        }
        x10 = (m * 10u + 804u) / 1609u;                               /* мили x10 */
        unit = 4;
    }
    if (unit == 5) { x10 = x10 / 4; unit = 4; }                       /* 1/4 мили -> мили */
    if (unit == 2) { x10 = x10 * 3; unit = 3; }                       /* ярды -> футы (в шрифте нет 'y') */
    const char *u = (unit == 0) ? t->m : (unit == 1) ? t->km : (unit == 2) ? "" :
                    (unit == 3) ? "ft" : (unit == 4) ? "mi" : "";
    if (unit == 0 || unit == 2 || unit == 3 || x10 >= 100)
        snprintf(b, n, "%lu%s", (unsigned long)(x10 / 10), u);
    else
        snprintf(b, n, "%lu.%lu%s", (unsigned long)(x10 / 10), (unsigned long)(x10 % 10), u);
}

/* ---------- выбор стрелки по BAP ManeuverDescriptor ----------
   Direction: 0x00 прямо, 0x40 налево, 0x80 назад, 0xC0 направо.
   Коды MainElement — из handoff; неизвестные рисуются поворотом по Direction
   и один раз печатаются в Serial, чтобы дописать таблицу. */
static const NavImg *nav_pick(const NavSet *set, uint8_t main, uint8_t dir)
{
    uint8_t sec   = (uint8_t)(((dir + 8) >> 4) & 15);
    bool    right = dir > 0x80;
    switch (main) {
    case 0x00: case 0x01: return NULL;                  /* нет символа / нет данных — ничего не рисуем */
    case 0x03: return &set->arrived[0];                 /* Arrived               */
    case 0x09: case 0x0A: return NULL;                  /* расчёт маршрута ("...") */
    case 0x0B: return &set->turn[0];                     /* FollowStreet          */
    case 0x0F: return &set->exit[1];                     /* ExitRight             */
    case 0x10: return &set->exit[0];                     /* ExitLeft              */
    case 0x13: case 0x14:                               /* «съезд»: штатная навигация шлёт съезд с дороги кодом 0x13/0x14,
                                                           приборка рисует его как съезд (прямая дорога + ответвление), а не
                                                           как раздвоение. Сторона по Direction, при 0 — по коду */
        if (dir == 0) right = (main == 0x13);
        return &set->exit[right ? 1 : 0];
    case 0x15: case 0x16: return &set->round[sec];       /* Roundabout            */
    case 0x19: return &set->uturn[right ? 1 : 0];        /* U-turn                */
    case 0x0D: case 0x1C: return &set->turn[sec];       /* Turn / PrepareTurn    */
    default: {
#if HUD_LOG_BAP
        static uint8_t reported[32];                    /* битовая карта 0..255  */
        if (!(reported[main >> 3] & (1u << (main & 7)))) {
            reported[main >> 3] |= 1u << (main & 7);
            arduino_printf("[nav] неизвестный MainElement 0x%02X (dir 0x%02X) — рисую поворотом\n", main, dir);
        }
#endif
        return &set->turn[sec];
    }
    }
}

/* Имя анимации в arrows.bin (tools/gen_arrow_anim.py) по коду манёвра и направлению.
   false — для этого манёвра анимации нет (рисуем обычную стрелку). */
static bool anim_name(char *out, size_t n, uint8_t main, uint8_t dir)
{
    bool right = dir > 0x80;
    int  deg   = (int)(((unsigned)dir * 360u + 128u) / 256u);         /* против часовой от «прямо», 0..360 */
    switch (main) {
    case 0x00: case 0x01: case 0x09: case 0x0A: return false;
    case 0x0B: snprintf(out, n, "approach"); return true;                           /* FollowStreet */
    case 0x03: snprintf(out, n, dir == 0 ? "arrived_c" : right ? "arrived_r" : "arrived_l"); return true;
    case 0x0F: snprintf(out, n, "exit_r"); return true;
    case 0x10: snprintf(out, n, "exit_l"); return true;
    case 0x13: case 0x14:
        if (dir == 0) right = (main == 0x13);
        snprintf(out, n, right ? "exit_r" : "exit_l");
        return true;
    case 0x19: snprintf(out, n, right ? "uturn_lht" : "uturn_rht"); return true;
    case 0x15: case 0x16: {                                                        /* кольцо: угол съезда по часовой, шаг 45° */
        int cw = (360 - deg) % 360;
        int e  = ((cw + 22) / 45) * 45;
        if (e == 0 || e >= 360) e = HUD_LEFT_HAND_TRAFFIC ? 45 : 315;              /* «прямо» — ближайший к нему съезд */
        snprintf(out, n, "roundabout_%s_e%03d", HUD_LEFT_HAND_TRAFFIC ? "lht" : "rht", e);
        return true;
    }
    default: {                                                                     /* поворот (и неизвестные коды, как у обычных стрелок) */
        int mag = right ? 360 - deg : deg;                                         /* 0..180 */
        if (mag >= 172) { snprintf(out, n, right ? "uturn_lht" : "uturn_rht"); return true; }
        int st = ((mag + 7) / 15) * 15;
        if (st == 0) snprintf(out, n, "approach");                                 /* «прямо» — как FollowStreet */
        else         snprintf(out, n, right ? "turn_r%03d" : "turn_l%03d", st);
        return true;
    }
    }
}

/* ---------- автояркость по Dimmung_01 ---------- */
/* освещённость, лк: LS_Helligkeit_FW x6 (до 6126); когда датчик в потолке —
   верхняя граница диапазона по RLS_Vorfeldhelligkeit_Boost (0 — ярче 24413) */
static int light_lux(const HudData *d)
{
    static const uint16_t boost_lux[16] = { 30000, 24413, 22193, 20176, 18342, 16647, 15158, 13780,
                                            12527, 11388, 10353, 9412, 8556, 7778, 7071, 6428 };
    if ((d->valid & V_RLS) && d->rls_fw <= 1021) {
        if (d->rls_fw >= 1020 && d->rls_boost <= 15) return boost_lux[d->rls_boost];
        return d->rls_fw * 6;
    }
    /* нет RLS_01 — свой фоторезистор (GPIO5), если включён и откалиброван */
    { int l = hud_light_lux(); if (l >= 0) return l; }
    /* иначе — оценка по авто-яркости дисплеев (0 лк -> ~99, 6000 лк -> 253) */
    if ((d->valid & V_DIM) && d->dim_raw <= 253)
        return d->dim_raw <= 99 ? 0 : (d->dim_raw - 99) * 6000 / 154;
    return -1;
}

static void update_brightness(const HudData *d, uint32_t now)
{
    static int cur = -1;
    hud_light_poll();
    int lux = light_lux(d);
    /* колёсико 0..1: BCM1_04; нет — DI_KL_58xd в темноте (там он = колёсико 10..100); нет — по умолчанию */
    float w;
    if (d->valid & V_WHEEL)                                   w = (d->wheel_pct - 1) / 99.0f;
    else if ((d->valid & V_DIM) && d->dim_raw <= 100 && lux <= 0) w = (d->dim_raw - 10) / 90.0f;
    else                                                      w = (HUD_WHEEL_DEFAULT - 1) / 99.0f;
    if (w < 0) w = 0;
    if (w > 1) w = 1;
    int target = HUD_BRIGHT_NO_DATA;
    float t = 0;
    if (lux >= 0) {
        /* освещённость 0..1, логарифмически между DARK и BRIGHT */
        float lo = log10f(HUD_LIGHT_LUX_DARK + 10.0f), hi = log10f(HUD_LIGHT_LUX_BRIGHT + 10.0f);
        t = (log10f(lux + 10.0f) - lo) / (hi - lo);
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        float dark   = HUD_BR_DARK_WHEEL_MIN   + (HUD_BR_DARK_WHEEL_MAX   - HUD_BR_DARK_WHEEL_MIN)   * w;
        float bright = HUD_BR_BRIGHT_WHEEL_MIN + (HUD_BR_BRIGHT_WHEEL_MAX - HUD_BR_BRIGHT_WHEEL_MIN) * w;
        target = (int)(dark + (bright - dark) * t + 0.5f);
    }
    if (now < 1500) return;                      /* подсветка ещё не инициализирована в setup() */
    int next = (cur < 0) ? target
             : (target > cur) ? (target - cur > HUD_BRIGHT_STEP ? cur + HUD_BRIGHT_STEP : target)
             : (cur - target > HUD_BRIGHT_STEP ? cur - HUD_BRIGHT_STEP : target);
    if (next != cur) {
        cur = next;
        setUpduty((uint16_t)(255 - cur));        /* ШИМ подсветки инвертирован: 0 = максимум */
    }
    /* диагностика: раз в 10 с, что пришло и что выставлено */
    static uint32_t last_log;
    if (HUD_LOG_DIM && now - last_log > 10000) {
        last_log = now;
        arduino_printf("[dim] свет %d лк (FW %s%u, Boost %u; фоторез. %d мВ) -> %.0f%%, колёсико %s%u%% -> подсветка %d/255\n",
                       lux, (d->valid & V_RLS) ? "" : "нет ", d->rls_fw * 6u, d->rls_boost, hud_light_mv(), t * 100.0f,
                       (d->valid & V_WHEEL) ? "" : "нет ", d->wheel_pct, cur);
    }
}

/* ============================================================================
   Обновление экрана — lv_timer, 50 мс, внутри задачи LVGL (под мьютексом)
   ========================================================================== */
/* ============================================================================
   Минималистичный режим (HUD_MODE_MIN)
   Слева — стрелка манёвра (последний кадр с перспективой) и расстояние под ней; по центру — скорость, под ней
   ОДИН знак по приоритету (камера > знак на дороге / ограничение с карты) и значок камеры с расстоянием; справа —
   иконка ACC / Lane Assist, значки Side Assist по бокам и заданная скорость под иконкой.
   Размеры шрифтов: tools/fonts/make_fonts.py (Overpass Medium, сжатие по ширине 0.90, цифры скорости 88 px).
   Данные те же, что у полного режима: контекст MinCtx заполняется в hud_update_cb.
   ========================================================================== */
LV_FONT_DECLARE(hud_font_mspeed);   /* скорость: Overpass Medium, цифры 88 px, сжато по ширине      */
LV_FONT_DECLARE(hud_font_mdist);    /* расстояние до манёвра, заданная скорость ACC: цифры 34 px    */
LV_FONT_DECLARE(hud_font_munit22);  /* «км/ч» рядом со скоростью                                    */
LV_FONT_DECLARE(hud_font_munit20);  /* единицы у расстояния и заданной скорости                     */
LV_FONT_DECLARE(hud_font_mcam);     /* расстояние до камеры                                         */
LV_FONT_DECLARE(hud_font_msign2);   /* цифры знака 56 px: 2 цифры (максимум, что влезает в белое поле) */
LV_FONT_DECLARE(hud_font_msign3);   /*   3 цифры                                                    */
LV_FONT_DECLARE(hud_font_mcdisc2);  /* цифры круга камеры: 2 цифры                                   */
LV_FONT_DECLARE(hud_font_mcdisc3);  /*   3 цифры                                                    */

#define MN_LZ_CX    102      /* ось левой зоны 0..205                */
#define MN_CZ_CX    320      /* ось центральной зоны 205..435        */
#define MN_RZ_CX    540      /* ось правой зоны 440..640             */
#define MN_ARROW_X  6        /* кадр стрелки 192x130: по центру левой зоны */
#define MN_ARROW_Y  2
#define MN_DIST_BASE 168     /* базовая линия расстояния и заданной скорости ACC */
#define MN_SPD_BASE  98      /* базовая линия скорости               */
#define MN_SIGN_D    56      /* знак / камера                        */
#define MN_SIGN_Y    113
#define MN_ICON_Y    4

#define MN_LANE_GREY   0xb0b4be
#define MN_LANE_DARK   0x46464a
#define MN_CAR_GREY    0x787878
#define MN_GREEN       0x2ee64a
#define MN_YELLOW      0xffc400
#define MN_SWA         0xe22020
#define MN_UNIT_SPEED  0x969696
#define MN_UNIT_SMALL  0xc8c8c8

static lv_obj_t *mn_box;
static lv_obj_t *mn_arrow, *mn_dist_num, *mn_dist_unit, *mn_calc;
static lv_obj_t *mn_spd, *mn_spd_unit, *mn_spd_ol[8];   /* mn_spd_ol — красная обводка цифр скорости при превышении */
static lv_obj_t *mn_sign, *mn_sign_lbl, *mn_sign_end, *mn_sign_car_l, *mn_sign_car_r;
static lv_obj_t *mn_cam_sq, *mn_cam_disc, *mn_cam_lbl, *mn_cam_eye, *mn_cam_dist;
static lv_obj_t *mn_acc_box, *mn_lane_l, *mn_lane_r, *mn_lead, *mn_arcs, *mn_own, *mn_swa_l, *mn_swa_r;
static lv_obj_t *mn_set_num, *mn_set_unit, *mn_lim;
static lv_obj_t *mn_link, *mn_pair;

typedef struct {
    bool        nav;                 /* навигация ведёт, поверх стрелки ничего нет */
    const NavImg *ni;               /* стрелка манёвра (NULL — нет символа) */
    bool        calc;                /* идёт расчёт маршрута */
    bool        acc_on, acc_set;
    uint8_t     acc_status;
    uint32_t    acc_col;
    bool        lim_on, lim_set;
    bool        lka_on, aca_on;
    bool        obj, obj_warn, arcs;
    uint8_t     swa_i, swa_w;
    int         smode;               /* знак по приоритету: SIGN_* */
    uint16_t    sval;
    bool        sign_warn;
    int         ov_opa;              /* превышение: 0..255 */
} MinCtx;

static lv_obj_t *mn_label(lv_obj_t *parent, const lv_font_t *f, uint32_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return l;
}

static lv_obj_t *mn_img(lv_obj_t *parent, const lv_img_dsc_t *src, int x, int y, uint32_t col)
{
    lv_obj_t *i = car_img(parent, src, x, y, col);
    lv_obj_add_flag(i, LV_OBJ_FLAG_HIDDEN);
    return i;
}

/* границы чернил строки (от начала надписи): первая и последняя ненулевые колонки глифов */
static void mn_ink(const lv_font_t *f, const char *s, int *x0, int *x1)
{
    int adv = 0, a = 0, b = 0;
    bool first = true;
    for (const char *p = s; *p; p++) {
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(f, &g, (uint32_t)(uint8_t)*p, 0)) continue;
        if (g.box_w > 0) {
            if (first) { a = adv + g.ofs_x; first = false; }
            b = adv + g.ofs_x + g.box_w;
        }
        adv += g.adv_w;
    }
    *x0 = a; *x1 = b;
}

/* надпись цифр: чернила по центру cx, базовая линия base; возвращает ширину чернил и левый край чернил */
static int mn_dx, mn_dy;                  /* смещение надписи (для обводки скорости), сбрасывается после вызова */
static int mn_digits(lv_obj_t *l, const lv_font_t *f, const char *s, int cx, int base, int *left)
{
    int a, b;
    set_text(l, s);
    mn_ink(f, s, &a, &b);
    int w = b - a;
    int x = cx - w / 2;                       /* левый край чернил */
    lv_obj_set_pos(l, x - a + mn_dx, base - (f->line_height - f->base_line) + mn_dy);
    vis(l, s[0] != 0);
    if (left) *left = x;
    return w;
}

/* цифры в круге: чернила по центру поля (cx, cy в координатах родителя) */
static void mn_center_ink(lv_obj_t *l, const lv_font_t *f, const char *s, int cx, int cy)
{
    int a, b;
    mn_ink(f, s, &a, &b);
    lv_font_glyph_dsc_t g;
    int top = 0, h = 0;
    if (lv_font_get_glyph_dsc(f, &g, '0', 0)) { h = g.box_h; top = (f->line_height - f->base_line) - g.ofs_y - g.box_h; }
    lv_obj_set_align(l, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(l, cx - (a + b) / 2, cy - (top + h / 2));
}

/* надпись на базовой линии, левый край чернил = x */
static void mn_text_at(lv_obj_t *l, const lv_font_t *f, const char *s, int x, int base)
{
    set_text(l, s);
    lv_obj_set_pos(l, x, base - (f->line_height - f->base_line));
    vis(l, s[0] != 0);
}

static int mn_text_w(const lv_font_t *f, const char *s)
{
    lv_point_t p = { 0, 0 };
    lv_txt_get_size(&p, s, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return p.x;
}

static lv_obj_t *mn_disc(lv_obj_t *parent, int d, int ring, int x, int y)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, d, d);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(C_SIGN_RED), 0);
    lv_obj_set_style_border_width(o, ring, 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void mn_build(lv_obj_t *scr)
{
    mn_box = lv_obj_create(scr);
    lv_obj_remove_style_all(mn_box);
    lv_obj_set_size(mn_box, 640, 172);
    lv_obj_set_pos(mn_box, 0, 0);
    lv_obj_clear_flag(mn_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* слева */
    mn_arrow = lv_img_create(mn_box);
    lv_img_set_src(mn_arrow, nav_big.turn[0].img);
    lv_obj_add_flag(mn_arrow, LV_OBJ_FLAG_HIDDEN);
    mn_dist_num  = mn_label(mn_box, &hud_font_mdist, 0xffffff);
    mn_dist_unit = mn_label(mn_box, &hud_font_munit20, MN_UNIT_SMALL);
    mn_calc      = mn_label(mn_box, &hud_font_mdist, C_GRAY);

    /* центр */
    for (int i = 0; i < 8; i++) mn_spd_ol[i] = mn_label(mn_box, &hud_font_mspeed, C_OVERSPEED);   /* под цифрами */
    mn_spd      = mn_label(mn_box, &hud_font_mspeed, 0xffffff);
    mn_spd_unit = mn_label(mn_box, &hud_font_munit22, MN_UNIT_SPEED);

    /* знак: красное кольцо, белое поле; конец ограничения — серая диагональ; «обгон запрещён» — две машины */
    mn_sign = mn_disc(mn_box, MN_SIGN_D, 6, MN_CZ_CX - MN_SIGN_D / 2, MN_SIGN_Y);
    mn_sign_lbl = lv_label_create(mn_sign);
    lv_label_set_text(mn_sign_lbl, "");
    lv_obj_set_style_text_font(mn_sign_lbl, &hud_font_msign2, 0);
    lv_obj_set_style_text_color(mn_sign_lbl, lv_color_black(), 0);
    lv_obj_center(mn_sign_lbl);
    {
        static lv_point_t end_pts[2] = { {13, MN_SIGN_D - 13}, {MN_SIGN_D - 13, 13} };
        mn_sign_end = lv_line_create(mn_sign);
        lv_line_set_points(mn_sign_end, end_pts, 2);
        lv_obj_set_style_line_width(mn_sign_end, 4, 0);
        lv_obj_set_style_line_color(mn_sign_end, lv_color_hex(C_GRAY), 0);
        lv_obj_set_pos(mn_sign_end, 0, 0);
        lv_obj_add_flag(mn_sign_end, LV_OBJ_FLAG_HIDDEN);
    }
    mn_sign_car_l = car_img(mn_sign, &img_sign_car, (MN_SIGN_D - 38) / 2 - 6,      (MN_SIGN_D - 15) / 2 - 6, C_SIGN_RED);
    mn_sign_car_r = car_img(mn_sign, &img_sign_car, (MN_SIGN_D - 38) / 2 - 6 + 20, (MN_SIGN_D - 15) / 2 - 6, 0x000000);
    lv_obj_add_flag(mn_sign_car_l, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(mn_sign_car_r, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(mn_sign, LV_OBJ_FLAG_HIDDEN);

    /* камера: жёлтый квадрат с круглым знаком лимита (80 %), справа значок камеры, под ним расстояние */
    mn_cam_sq = lv_obj_create(mn_box);
    lv_obj_remove_style_all(mn_cam_sq);
    lv_obj_set_size(mn_cam_sq, MN_SIGN_D, MN_SIGN_D);
    lv_obj_set_pos(mn_cam_sq, MN_CZ_CX - MN_SIGN_D / 2, MN_SIGN_Y);
    lv_obj_set_style_radius(mn_cam_sq, 6, 0);
    lv_obj_set_style_bg_color(mn_cam_sq, lv_color_hex(MN_YELLOW), 0);
    lv_obj_set_style_bg_opa(mn_cam_sq, LV_OPA_COVER, 0);
    lv_obj_clear_flag(mn_cam_sq, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    mn_cam_disc = mn_disc(mn_cam_sq, 44, 5, 6, 6);
    mn_cam_lbl = lv_label_create(mn_cam_disc);
    lv_label_set_text(mn_cam_lbl, "");
    lv_obj_set_style_text_font(mn_cam_lbl, &hud_font_mcdisc2, 0);
    lv_obj_set_style_text_color(mn_cam_lbl, lv_color_black(), 0);
    lv_obj_add_flag(mn_cam_sq, LV_OBJ_FLAG_HIDDEN);
    mn_cam_eye  = mn_img(mn_box, &img_cam_icon, 0, 0, MN_YELLOW);   /* тот же значок камеры, что и в полном режиме (38x29) */
    mn_cam_dist = mn_label(mn_box, &hud_font_mcam, MN_YELLOW);

    /* справа: иконка ACC (слои-маски), значки Side Assist, заданная скорость */
    mn_acc_box = lv_obj_create(mn_box);
    lv_obj_remove_style_all(mn_acc_box);
    lv_obj_set_size(mn_acc_box, M_ACC_W, M_ACC_H);
    lv_obj_set_pos(mn_acc_box, MN_RZ_CX - M_ACC_W / 2, MN_ICON_Y);
    lv_obj_clear_flag(mn_acc_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    mn_lane_l = car_img(mn_acc_box, &img_m_lane_l, M_LANE_L_X, M_LANE_L_Y, MN_LANE_GREY);
    mn_lane_r = car_img(mn_acc_box, &img_m_lane_r, M_LANE_R_X, M_LANE_R_Y, MN_LANE_GREY);
    mn_lead   = car_img(mn_acc_box, &img_m_lead,   M_LEAD_X,   M_LEAD_Y,   MN_GREEN);
    mn_arcs   = car_img(mn_acc_box, &img_m_arcs,   M_ARCS_X,   M_ARCS_Y,   MN_GREEN);
    mn_own    = car_img(mn_acc_box, &img_m_own,    M_OWN_X,    M_OWN_Y,    MN_GREEN);
    lv_obj_add_flag(mn_acc_box, LV_OBJ_FLAG_HIDDEN);
    mn_swa_l = mn_img(mn_box, &img_m_swa, MN_RZ_CX - 82 - 13, 24, MN_SWA);
    mn_swa_r = mn_img(mn_box, &img_m_swa, MN_RZ_CX + 82 - 13, 24, MN_SWA);
    mn_set_num  = mn_label(mn_box, &hud_font_mdist, 0xffffff);
    mn_set_unit = mn_label(mn_box, &hud_font_munit20, MN_UNIT_SMALL);
    mn_lim = mn_img(mn_box, &img_limiter, MN_RZ_CX - 15, 60, 0xffffff);

    /* нет связи */
    mn_link = mn_label(mn_box, LV_FONT_DEFAULT, 0x666666);
    mn_pair = mn_label(mn_box, &hud_font_menu, C_ORANGE);
    lv_label_set_long_mode(mn_pair, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(mn_pair, 344);
    lv_obj_set_pos(mn_pair, 148, 34);
    lv_obj_add_flag(mn_box, LV_OBJ_FLAG_HIDDEN);
}

static void mn_update(const HudData *d, uint32_t now, const MinCtx *c)
{
    char buf[40];
    const HudTxt *tx = txt();

    /* ---- левая зона: стрелка и расстояние ---- */
    {
        static const NavImg *last = NULL;
        bool show = c->nav && c->ni != NULL;
        if (show && c->ni != last) {
            lv_img_set_src(mn_arrow, c->ni->img);
            lv_obj_set_pos(mn_arrow, MN_ARROW_X + c->ni->x, MN_ARROW_Y + c->ni->y);
        }
        last = c->ni;
        vis(mn_arrow, show);
        bool dv = show && (d->valid & V_MDIST);
        if (dv) {
            char s[24];
            fmt_dist(s, sizeof s, d->man_dist_x10, d->man_unit);
            int k = 0;
            while (s[k] && ((s[k] >= '0' && s[k] <= '9') || s[k] == '.')) k++;
            char num[16], unit[12];
            snprintf(num, sizeof num, "%.*s", k, s);
            snprintf(unit, sizeof unit, "%s", s + k);
            int left;
            int w = mn_digits(mn_dist_num, &hud_font_mdist, num, MN_LZ_CX, MN_DIST_BASE, &left);
            mn_text_at(mn_dist_unit, &hud_font_munit20, unit, left + w + 7, MN_DIST_BASE);
        } else {
            vis(mn_dist_num, false);
            vis(mn_dist_unit, false);
        }
        if (c->nav && c->calc) mn_digits(mn_calc, &hud_font_mdist, "...", MN_LZ_CX, 80, NULL);
        else                   vis(mn_calc, false);
    }

    /* ---- центр: скорость, «км/ч», знак / камера ---- */
    {
        if (d->valid & V_SPEED) {
            snprintf(buf, sizeof buf, "%u", spd(d->speed_kmh));
            int left;
            int w = mn_digits(mn_spd, &hud_font_mspeed, buf, MN_CZ_CX, MN_SPD_BASE, &left);
            /* превышение — красная обводка вокруг белых цифр, как в полном режиме */
            static const int8_t off[8][2] = { {-3,0},{3,0},{0,-3},{0,3},{-2,-2},{2,-2},{-2,2},{2,2} };
            for (int i = 0; i < 8; i++) {
                if (c->ov_opa > 0) {
                    mn_dx = off[i][0]; mn_dy = off[i][1];
                    mn_digits(mn_spd_ol[i], &hud_font_mspeed, buf, MN_CZ_CX, MN_SPD_BASE, NULL);
                    mn_dx = mn_dy = 0;
                    if (lv_obj_get_style_text_opa(mn_spd_ol[i], LV_PART_MAIN) != c->ov_opa) lv_obj_set_style_text_opa(mn_spd_ol[i], (lv_opa_t)c->ov_opa, 0);
                }
                vis(mn_spd_ol[i], c->ov_opa > 0);
            }
            mn_text_at(mn_spd_unit, &hud_font_munit22, g_units == HUD_UNITS_MI ? "mph" : tx->kmh, left + w + 12, MN_SPD_BASE);
        } else {
            vis(mn_spd, false);
            for (int i = 0; i < 8; i++) vis(mn_spd_ol[i], false);
            vis(mn_spd_unit, false);
        }

        bool cam = g_cam_hit;
        vis(mn_cam_sq, cam);
        vis(mn_cam_eye, cam);
        vis(mn_cam_dist, cam);
        if (cam) {
            snprintf(buf, sizeof buf, "%u", g_cam_lim);
            const lv_font_t *cf = g_cam_lim >= 100 ? &hud_font_mcdisc3 : &hud_font_mcdisc2;
            set_font(mn_cam_lbl, cf);
            set_text(mn_cam_lbl, g_cam_lim ? buf : "");
            mn_center_ink(mn_cam_lbl, cf, buf, 17, 17);          /* поле круга 34 px */
            bool ph = g_cam_over && (now % 400) < 200;
            set_bg_color(mn_cam_disc, ph ? C_SIGN_WARN : 0xffffff);
            set_img_color(mn_cam_eye, g_cam_over ? 0xff3030 : MN_YELLOW);
            /* значок камеры справа от квадрата, расстояние под ним, низ блока на y=170 */
            int tw = mn_text_w(&hud_font_mcam, g_cam_txt);
            int bw = tw > 46 ? tw : 46;
            int bx = MN_CZ_CX + MN_SIGN_D / 2 + 14, by = 170 - (40 + 4 + 22);
            lv_obj_set_pos(mn_cam_eye, bx + (bw - 38) / 2, by + 40 - 29);   /* низ значка там же, где был низ прежнего (40 px) */
            set_text(mn_cam_dist, g_cam_txt);
            lv_obj_set_pos(mn_cam_dist, bx + (bw - tw) / 2, by + 40 + 4 + 20 - (hud_font_mcam.line_height - hud_font_mcam.base_line));
            set_text_color(mn_cam_dist, g_cam_over ? 0xff3030 : MN_YELLOW);
        }

        bool sg = !cam && c->smode != SIGN_HIDE && c->smode != SIGN_RAW;
        vis(mn_sign, sg);
        if (sg) {
            bool wr = HUD_SIGN_WARN_FLASH && c->sign_warn && (now % 500) < 250;
            set_bg_color(mn_sign, wr ? C_SIGN_WARN : 0xffffff);
            set_border_color(mn_sign, (c->smode == SIGN_LIMIT || c->smode == SIGN_NOOVT) ? C_SIGN_RED : C_GRAY);
            vis(mn_sign_end, c->smode == SIGN_END);
            vis(mn_sign_car_l, c->smode == SIGN_NOOVT);
            vis(mn_sign_car_r, c->smode == SIGN_NOOVT);
            if (c->smode == SIGN_LIMIT) {
                snprintf(buf, sizeof buf, "%u", c->sval);
                const lv_font_t *sf = c->sval >= 100 ? &hud_font_msign3 : &hud_font_msign2;
                set_font(mn_sign_lbl, sf);
                set_text(mn_sign_lbl, buf);
                mn_center_ink(mn_sign_lbl, sf, buf, 22, 22);     /* поле знака 44 px */
            } else {
                set_text(mn_sign_lbl, "");
            }
        }
    }

    /* ---- правая зона: иконка ACC, Side Assist, заданная скорость ---- */
    {
        bool swa_any = (c->swa_i | c->swa_w) != 0;
        bool icon = c->lim_on ? swa_any : (c->acc_on || c->lka_on || c->aca_on || c->obj_warn || swa_any);   /* при лимитере — только серые полосы под Side Assist */
        vis(mn_acc_box, icon);
        if (icon) {
            uint32_t car = c->acc_on ? (c->acc_status == 3 ? MN_GREEN : c->acc_status == 4 ? 0xffffff : MN_CAR_GREY) : MN_CAR_GREY;
            uint32_t lane_idle = ((c->acc_on && c->acc_status >= 3) || swa_any) ? MN_LANE_GREY : MN_LANE_DARK;   /* Side Assist — серые полосы */
            for (int side = 0; side < 2; side++) {
                uint8_t lv  = side ? d->lka_line_r : d->lka_line_l;
                bool    wr  = d->lka_warn & (side ? 2 : 1);
                uint32_t col;
                if (c->lka_on) {
                    if (wr || lv == 3)  col = ((now % 500) < 250) ? C_ORANGE : MN_LANE_DARK;
                    else if (lv == 2)   col = MN_GREEN;
                    else                col = MN_YELLOW;
                } else if (c->aca_on)   col = MN_GREEN;
                else                    col = lane_idle;
                set_img_color(side ? mn_lane_r : mn_lane_l, col);
            }
            bool veh = !c->lim_on;                       /* при лимитере машин и радара нет — только полосы */
            vis(mn_own, veh);
            if (veh) set_img_color(mn_own, car);
            vis(mn_lead, veh && c->obj);
            if (veh && c->obj) set_img_color(mn_lead, c->obj_warn ? C_OBJ_WARN : car);
            vis(mn_arcs, veh && c->arcs);
            if (veh && c->arcs) set_img_color(mn_arcs, car);
        }
        bool tl = (c->swa_i | c->swa_w) & 1, tr = (c->swa_i | c->swa_w) & 2;
        if (c->swa_w) { bool ph = (now % 400) < 200; tl = tl && ph; tr = tr && ph; }
        vis(mn_swa_l, tl);
        vis(mn_swa_r, tr);

        /* заданная скорость ACC / лимитера под иконкой */
        bool lim_num = c->lim_on && c->lim_set;
        bool acc_num = !c->lim_on && c->acc_on && c->acc_set;
        vis(mn_lim, c->lim_on);
        if (c->lim_on) set_img_color(mn_lim, c->lim_set ? 0xffffff : C_GRAY);
        if (lim_num || acc_num) {
            unsigned raw = lim_num ? d->limiter_raw : d->acc_set_raw;
            snprintf(buf, sizeof buf, "%u", spd((raw * 32u + 50u) / 100u));
            bool dim = acc_num && d->acc_status == 2;
            int left;
            int w = mn_digits(mn_set_num, &hud_font_mdist, buf, MN_RZ_CX, MN_DIST_BASE, &left);
            set_text_color(mn_set_num, dim ? 0x828282 : 0xffffff);
            set_text_color(mn_set_unit, dim ? 0x6e6e6e : MN_UNIT_SMALL);
            mn_text_at(mn_set_unit, &hud_font_munit20, g_units == HUD_UNITS_MI ? "mph" : tx->kmh, left + w + 7, MN_DIST_BASE);
        } else {
            vis(mn_set_num, false);
            vis(mn_set_unit, false);
        }
    }

    /* ---- нет связи ---- */
    {
        bool pf = !d->link_up && hud_src_pair_failed();
        if (pf) set_text(mn_pair, g_lang == HUD_LANG_EN
            ? "Pairing rejected. Check the passkey or reset pairings on the device: hold BOOT for 5 s."
            : "Сопряжение не принято. Проверьте код доступа или сбросьте сопряжения на устройстве: удерживайте BOOT 5 с.");
        vis(mn_pair, pf);
        bool ls = false;   /* без связи — заставка */
        if (ls) {
            set_text(mn_link, LV_SYMBOL_BLUETOOTH " ...");
            lv_obj_set_pos(mn_link, MN_CZ_CX - mn_text_w(LV_FONT_DEFAULT, LV_SYMBOL_BLUETOOTH " ...") / 2, 4);
        }
        vis(mn_link, ls);
    }
}

/* переключение режима: полный экран <-> минималистичный; значки двери / pre sense живут поверх обоих и встают на место стрелки */
static void mode_apply(void)
{
    bool mn = g_mode == HUD_MODE_MIN;
    vis(full_root, !mn);
    vis(mn_box, mn);
    lv_obj_set_x(car_box, mn ? MN_ARROW_X : 145);
    lv_obj_set_x(ps_box,  mn ? MN_ARROW_X : 145);
}

/* Анимация стрелки: цикл всех кадров манёвра за HUD_ANIM_PERIOD_MS по кругу. Кадр выбирается по времени; зовётся и из
   hud_update_cb (50 мс), и из быстрого таймера anim_timer_cb (16 мс) — иначе кадры привязаны к сетке 50 мс и идут рывками. */
static int      g_anim_run = -1;                 /* индекс играющей анимации или -1 (пишет hud_update_cb) */
static int      an_la = -1, an_lf = -1;
static uint32_t an_t0;

static bool anim_step(int aidx, uint32_t tnow)
{
    int nf = hud_anim_frames(aidx);
    if (nf <= 0) return false;
    if (aidx != an_la) an_t0 = tnow;                                      /* смена манёвра — цикл с первого кадра */
    int fr = (int)((((tnow - an_t0) % HUD_ANIM_PERIOD_MS) * (uint32_t)nf) / HUD_ANIM_PERIOD_MS);
    if (aidx == an_la && fr == an_lf) return true;
    const lv_img_dsc_t *fd = hud_anim_frame(aidx, fr);
    if (!fd) { an_la = -1; an_lf = -1; return false; }
    lv_img_set_src(anim_img, fd);
    an_la = aidx; an_lf = fr;
    return true;
}

static void anim_timer_cb(lv_timer_t *t)
{
    (void)t;
    int ai = g_anim_run;
    if (ai >= 0 && hud_anim_ready()) anim_step(ai, hud_now_ms());
}

#if HUD_LOG_MEM
/* раз в 10 с: память LVGL и кучи, запас стека задачи LVGL — чтобы по журналу отличить утечку/фрагментацию от других причин перезагрузки */
static void mem_timer_cb(lv_timer_t *t)
{
    (void)t;
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    unsigned stk = 0;
#ifdef ESP_PLATFORM
    TaskHandle_t th = xTaskGetHandle("LVGL");
    if (th) stk = (unsigned)uxTaskGetStackHighWaterMark(th) * sizeof(StackType_t);
#endif
    arduino_printf("[mem] LVGL занято %u из %u Б, пик %u Б, фрагментация %u%%, крупнейший свободный %u Б; куча свободно %u Б (мин. %u Б); стек LVGL свободно (мин.) %u Б\n",
                   (unsigned)(m.total_size - m.free_size), (unsigned)m.total_size, (unsigned)m.max_used, (unsigned)m.frag_pct,
                   (unsigned)m.free_biggest_size, (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(), stk);
}
#endif

/* ---------- заставка, пока нет связи ---------- */
/* Весь экран: «HUD VAG» крупно, тонкая линия с градиентом, подпись платформы (MLB EVO / MQB — по HUD_PLATFORM), внизу мелкий
   серый значок Bluetooth. Появляется, только если связи (BLE / свой CAN) нет дольше SPLASH_DELAY_MS — короткие пропадания
   не мигают, плавно (0,7 с); как только связь есть (link_up) — пропадает сразу. Если сопряжение отклонено — вместо подписи
   и значка показывается подсказка про код доступа. Меню настроек и экран обновления лежат поверх заставки. */
#if HUD_PLATFORM == HUD_PLATFORM_MQB
#define SPLASH_PLAT_DEFAULT "MQB"
#else
#define SPLASH_PLAT_DEFAULT "MLB EVO"
#endif
#define SPLASH_W          640
#define SPLASH_H          172
#define SPLASH_DELAY_MS   1500    /* столько связи не должно быть, чтобы заставка появилась */
#define SPLASH_TITLE_CY   58      /* центр заглавных «HUD VAG» по вертикали */
#define SPLASH_LINE_Y     100
#define SPLASH_SUB_CY     122
#define SPLASH_BT_Y       148
#define SPLASH_TITLE_LS   9       /* разрядка, px */
#define SPLASH_SUB_LS     8
#define C_SPLASH_TITLE    0xf2f5f8
#define C_SPLASH_SUB      0x9db0c4
static char      g_splash_plat[24] = SPLASH_PLAT_DEFAULT;
static lv_obj_t *splash, *splash_in, *splash_sub, *splash_bt, *splash_msg;

void hud_set_splash_plat(const char *s)
{
    if (s && *s) { strncpy(g_splash_plat, s, sizeof g_splash_plat - 1); g_splash_plat[sizeof g_splash_plat - 1] = 0; }
}

static lv_obj_t *splash_label(lv_obj_t *par, const lv_font_t *f, const char *s, int ls, uint32_t col, int cap_cy)
{
    lv_obj_t *l = lv_label_create(par);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_style_text_letter_space(l, ls, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, SPLASH_W);
    lv_label_set_text(l, s);
    lv_font_glyph_dsc_t g; int top = 0, h = 0;
    if (lv_font_get_glyph_dsc(f, &g, 'H', 0)) { h = g.box_h; top = (f->line_height - f->base_line) - g.ofs_y - g.box_h; }
    lv_obj_set_pos(l, 0, cap_cy - (top + h / 2));
    return l;
}

static void splash_rule(lv_obj_t *par, int x, int w, bool rise)
{
    lv_obj_t *o = lv_obj_create(par);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(o, w, 2);
    lv_obj_set_pos(o, x, SPLASH_LINE_Y);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(rise ? 0x000000 : C_NAV_ARROW), 0);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(rise ? C_NAV_ARROW : 0x000000), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_HOR, 0);
}

static void splash_build(lv_obj_t *scr)
{
    splash = lv_obj_create(scr);
    lv_obj_remove_style_all(splash);
    lv_obj_set_size(splash, SPLASH_W, SPLASH_H);
    lv_obj_set_pos(splash, 0, 0);
    lv_obj_set_style_bg_color(splash, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(splash, LV_OPA_COVER, 0);
    lv_obj_clear_flag(splash, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    splash_in = lv_obj_create(splash);              /* содержимое — отдельным слоем, чтобы плавно проявлять его на чёрном */
    lv_obj_remove_style_all(splash_in);
    lv_obj_set_size(splash_in, SPLASH_W, SPLASH_H);
    lv_obj_clear_flag(splash_in, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    splash_label(splash_in, &hud_font_splash, "HUD VAG", SPLASH_TITLE_LS, C_SPLASH_TITLE, SPLASH_TITLE_CY);
    splash_rule(splash_in, SPLASH_W / 2 - 180, 180, true);
    splash_rule(splash_in, SPLASH_W / 2, 180, false);
    splash_sub = splash_label(splash_in, &hud_font_splash_sub, g_splash_plat, SPLASH_SUB_LS, C_SPLASH_SUB, SPLASH_SUB_CY);
    splash_bt = lv_label_create(splash_in);          /* мелкий значок связи внизу */
    lv_obj_set_style_text_font(splash_bt, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(splash_bt, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_align(splash_bt, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(splash_bt, SPLASH_W);
    lv_label_set_text(splash_bt, LV_SYMBOL_BLUETOOTH " ...");
    lv_obj_set_pos(splash_bt, 0, SPLASH_BT_Y);
    splash_msg = lv_label_create(splash_in);         /* сопряжение отклонено — подсказка вместо подписи */
    lv_label_set_long_mode(splash_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(splash_msg, &hud_font_menu, 0);
    lv_obj_set_style_text_color(splash_msg, lv_color_hex(C_ORANGE), 0);
    lv_obj_set_style_text_align(splash_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(splash_msg, 580);
    lv_obj_set_pos(splash_msg, (SPLASH_W - 580) / 2, SPLASH_LINE_Y + 12);
    lv_obj_add_flag(splash_msg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(splash, LV_OBJ_FLAG_HIDDEN);
}

static void splash_fade_cb(void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }

static void splash_show(bool on)
{
    bool was = !lv_obj_has_flag(splash, LV_OBJ_FLAG_HIDDEN);
    if (on && !was) {
        lv_obj_clear_flag(splash, LV_OBJ_FLAG_HIDDEN);
        lv_anim_t a; lv_anim_init(&a);
        lv_anim_set_var(&a, splash_in); lv_anim_set_exec_cb(&a, splash_fade_cb);
        lv_anim_set_values(&a, 0, 255); lv_anim_set_time(&a, 700);
        lv_anim_start(&a);
    } else if (!on && was) {
        lv_anim_del(splash_in, splash_fade_cb);
        lv_obj_add_flag(splash, LV_OBJ_FLAG_HIDDEN);   /* связь есть — убираем сразу */
    }
}

static void hud_update_cb(lv_timer_t *t)
{
    (void)t;
    HudData d;
    hud_data_snapshot(&d);
    uint32_t now = hud_now_ms();
    char buf[32];
    (void)now;
    uint16_t lim_kmh_for_speed = 0;
    int ov_opa = 0;                    /* превышение 0..255 — для минималистичного режима */
    static uint8_t mode_shown = 0xFF;
    if (g_mode != mode_shown) { mode_shown = g_mode; mode_apply(); }

    {   /* заставка: связи нет дольше SPLASH_DELAY_MS (в режиме обновления её нет — там свой экран) */
        static bool nl_run; static uint32_t nl_since;
        bool nl = !d.link_up && !hud_ota_mode();
        if (!nl) nl_run = false; else if (!nl_run) { nl_run = true; nl_since = now; }
        bool sp = nl && (uint32_t)(now - nl_since) >= SPLASH_DELAY_MS;
        splash_show(sp);
        if (sp) {
            bool spf = hud_src_pair_failed();
            if (spf) set_text(splash_msg, g_lang == HUD_LANG_EN
                ? "Pairing rejected. Check the passkey or reset pairings on the device: hold BOOT for 5 s."
                : "Сопряжение не принято. Проверьте код доступа или сбросьте сопряжения на устройстве: удерживайте BOOT 5 с.");
            vis(splash_msg, spf);
            vis(splash_sub, !spf);
            vis(splash_bt, !spf);
            set_text(splash_sub, g_splash_plat);
            update_brightness(&d, now);
            return;
        }
    }

    /* --- подпись единиц скорости (язык / единицы могут смениться на ходу) --- */
    set_text(kmh_lbl, g_units == HUD_UNITS_MI ? "mph" : txt()->kmh);

    /* --- полоска расхода топлива: правый край, снизу вверх, 0..HUD_FUEL_FULL л/100 км --- */
#if HUD_FUEL_BAR
    {
        static bool fast = false;                        /* гистерезис по скорости */
        if (!(d.valid & V_SPEED))                    fast = false;
        else if (d.speed_kmh >= HUD_FUEL_ON_KMH)     fast = true;
        else if (d.speed_kmh <  HUD_FUEL_OFF_KMH)    fast = false;
        /* средний — «с момента запуска» бортового компьютера приборки (BAP_BC 0x18),
           если его нет — свой средний по счётчику топлива; показывается с любой скорости.
           мгновенный — по счётчику Motor_04, с HUD_FUEL_ON_KMH */
        uint16_t fv = 0; bool show = false;
        if (g_fmode == 1) {
            if ((d.valid & V_BC) && d.bc_avg_x10 != 0xFFFF)          { fv = d.bc_avg_x10;   show = true; }
            else if ((d.valid & V_FUEL) && d.fuel_avg_x10 != 0xFFFF) { fv = d.fuel_avg_x10; show = true; }
        } else if (g_fmode == 0) {
            fv = d.fuel_l100_x10; show = (d.valid & V_FUEL) && fast;
        }                                                         /* 2 — выкл: show остаётся false */
        if (show) {
            uint32_t full_x10 = HUD_FUEL_FULL * 10u;
            int h;
            if (fv > full_x10) {                                  /* больше максимума — мигает целиком */
                h = 172;
                show = (now % 500) < 250;
            } else {
                h = (int)(fv * 172u / full_x10);
                if (h < 1) h = 1;
            }
            if (lv_obj_get_height(fuel_bar) != h) {
                lv_obj_set_height(fuel_bar, h);
                lv_obj_set_y(fuel_bar, 172 - h);
            }
        }
        vis(fuel_bar, show);
    }
#endif

    /* --- связь, яркость --- */
    /* BLE-сопряжение не удаётся — подсказка вместо значка связи */
    bool pf = !d.link_up && hud_src_pair_failed();
    if (pf) set_text(pair_lbl, g_lang == HUD_LANG_EN
        ? "Pairing rejected. Check the passkey or reset pairings on the device: hold BOOT for 5 s."
        : "Сопряжение не принято. Проверьте код доступа или сбросьте сопряжения на устройстве: удерживайте BOOT 5 с.");
    vis(pair_lbl, pf);
    link_pf = pf;      /* значок источника обновляется ниже, после TSR (они делят место в верхней строке) */
    update_brightness(&d, now);

    /* --- скорость --- */
    if (d.valid & V_SPEED) {
        snprintf(buf, sizeof buf, "%u", spd(d.speed_kmh));
        set_text(speed_lbl, buf);
        vis(speed_lbl, true);
    } else {
        if (HUD_LOG_HUD && !lv_obj_has_flag(speed_lbl, LV_OBJ_FLAG_HIDDEN))
            arduino_printf("[hud] скорость погасла: нет кадра 0x30B > таймаута (link=%d, потерь=%lu)\n",
                           d.link_up, (unsigned long)d.batches_lost);
        vis(speed_lbl, false);
    }

    /* --- ACC / лимитер: значок + заданная скорость ---
       Режимы не пересекаются: при лимитере ACC_Status_Anzeige = 0. */
    bool acc_v   = d.valid & V_ACC;
    bool acc_set = d.acc_set_raw != 0 && d.acc_set_raw < 0x3FF;      /* скорость ACC задана */
    /* ACC показываем, когда он регулирует (3) или водитель превышает (4),
       а в пассиве (2) — только если скорость уже задана. Пассив без скорости —
       ни значка, ни числа, ни машин на значке ACC/LKA. */
    bool acc_on  = acc_v && (d.acc_status == 3 || d.acc_status == 4 ||
                             (d.acc_status == 2 && acc_set));
    uint32_t acc_col = (d.acc_status == 3) ? C_GREEN : (d.acc_status == 4) ? 0xffffff : C_GRAY;
    bool lim_on  = (d.valid & V_LIM) && d.limiter_raw != 1022;   /* 1022 = выкл, 1023 = вкл без скорости */
    bool lim_set = lim_on && d.limiter_raw < 1022;

    if (lim_on) {
        set_img_color(lim_icon, lim_set ? 0xffffff : C_GRAY);
        if (lim_set) {
            snprintf(buf, sizeof buf, "%u", spd((d.limiter_raw * 32u + 50u) / 100u));
            set_text(acc_speed_lbl, buf);
            set_text_color(acc_speed_lbl, 0xffffff);
        }
        vis(lim_icon, true);
        vis(acc_dot, false);
        vis(acc_speed_lbl, lim_set);
    } else {
        vis(lim_icon, false);
        bool acc_num = acc_on && acc_set;
        if (acc_on) set_img_color(acc_dot, acc_col);
        vis(acc_dot, acc_on);
        if (acc_num) {
            snprintf(buf, sizeof buf, "%u", spd((d.acc_set_raw * 32u + 50u) / 100u));
            set_text(acc_speed_lbl, buf);
            /* пассив — приглушённо, активен — ярко */
            set_text_color(acc_speed_lbl, d.acc_status == 2 ? 0x8a8a8a : 0xe6e6e6);
        }
        vis(acc_speed_lbl, acc_num);
    }

    /* --- режим пробки (STA_Primaeranz): 1 готов, 2 активен, 3 предупреждение --- */
    bool jam = !lim_on && (d.valid & V_JAM) && d.jam_state >= 1;
    if (jam) set_img_color(jam_icon, d.jam_state == 2 ? C_GREEN : d.jam_state == 3 ? C_ORANGE : C_GRAY);
    vis(jam_icon, jam);

    /* --- значок ACC / Lane Assist ---
       своя машина: есть, если включён ACC или LKA
       машина впереди: ACC включён; волны: ACC регулирует (3) / водитель превышает (4)
       линии: LKA, цвет — по уровню каждой линии (LDW_02): 2 зелёная (линия распознана),
       иначе жёлтая (не распознана), оранжевая (мигает) — выход за линию;
       общий признак LKA_PASSIVE цвет не задаёт (в движении бит 62 не приходит) */
    bool lka_on = (d.valid & V_LKA) && d.lka_state != LKA_OFF;
    bool aca_on = acc_on && d.aca_lane == 2;          /* удержание в полосе в составе ACC */
    bool lines  = lka_on || aca_on;
    /* машина впереди (ACC_Relevantes_Objekt_02):
       1 — цель ACC: только при включённом ACC, цветом ACC;
       2 — предупреждение (опасная дистанция): ВСЕГДА, даже при выключенном ACC, красным */
    bool obj_warn = (d.valid & V_ACC) && d.acc_object == 2;
    bool obj = obj_warn || (acc_on && d.acc_object == 1);
    vis(as_front, obj);
    if (obj) set_img_color(as_front, obj_warn ? C_OBJ_WARN : acc_col);
    /* радар (волны): пока ACC регулирует (status 3, машинка зелёная) — всегда, даже без машины впереди;
       при status 4 (значок белый) — только когда впереди есть цель, как раньше */
    bool arcs = (acc_on && d.acc_status == 3) || (obj && d.acc_status == 4);
    vis(as_arcs, arcs);
    if (arcs) set_img_color(as_arcs, acc_col);

    /* цвет своей машины задаёт только ACC; состояние Lane Assist показывают боковые полосы */
    uint32_t own_col = acc_on ? acc_col : C_GRAY;
    vis(as_own, acc_on || obj_warn);        /* Lane Assist своей машинки не вызывает */
    set_img_color(as_own, own_col);

    /* Side Assist: машина сбоку — полоска этой стороны красная (поверх цвета LKA),
       попытка перестроения — мигает красным + звук. Работает и без LKA / ACC. */
    uint8_t swa_i = 0, swa_w = 0;
#if HUD_SWA
    if (d.valid & V_SWA) { swa_i = d.swa_info; swa_w = d.swa_warn; }
#endif
    bool swa_any = (swa_i | swa_w) != 0;

    for (int side = 0; side < 2; side++) {
        lv_obj_t *ln = side ? as_line_r : as_line_l;
        uint8_t  lv  = side ? d.lka_line_r : d.lka_line_l;
        bool     wr  = d.lka_warn & (side ? 2 : 1);
        uint8_t  bit = side ? 2 : 1;
        uint32_t col;
        if (swa_w & bit)                          col = ((now % 400) < 200) ? C_SWA : 0x000000;
        else if (swa_i & bit)                     col = C_SWA;
        else if (!lka_on)                         col = C_GREEN;      /* только ACA */
        else if (wr || lv == 3)                   col = ((now % 500) < 250) ? C_ORANGE : 0x000000;
        else if (lv == 2)                         col = C_GREEN;
        else                                      col = C_YELLOW;
        bool show = lines || ((swa_i | swa_w) & bit);
        if (show) set_img_color(ln, col);
        vis(ln, show);
    }
    if (swa_any) vis(as_own, true);                       /* своя машина — для понятности, где «сбоку» */
    vis(assist_box, acc_on || lines || obj_warn || swa_any);

#if HUD_SWA && HUD_SWA_BEEP
    {   /* звук: на появлении предупреждения и дальше раз в HUD_SWA_BEEP_REPEAT_MS */
        static uint32_t last_beep = 0;
        static uint8_t  prev_w = 0;
        if (swa_w && (!prev_w || now - last_beep >= HUD_SWA_BEEP_REPEAT_MS)) {
            hud_sound_play(hud_snd_swa_beep, hud_snd_swa_beep_len);
            last_beep = now;
        }
        prev_w = swa_w;
    }
#endif

    /* --- знаки ---
       Основной знак, по приоритету: PSD явный знак -> VZE_01 -> PSD по правилам.
       Смена PSD явный -> по правилам — с задержкой HUD_PSD_LEGAL_DELAY_MS (мерцание
       при перестройке дерева сегментов). Дополнительные знаки (VZE 2/3, запрет
       обгона) показываются по очереди с основным, каждый HUD_SIGN_CYCLE_MS. */
    SignItem items[4]; int n_items = 0;
    {
        static uint32_t pe_t = 0; static uint16_t pe_kmh = 0; static bool pe_novt = false;
        psd_limit_t P = { 0, PSD_LIM_NONE, false };
        if (g_psd) {
            P = psd_get(now, 3000);
            if (P.src == PSD_LIM_EXPLICIT) { pe_t = now ? now : 1; pe_kmh = P.kmh; pe_novt = P.no_overtake; }
        } else {
            pe_t = 0;
        }
        bool psd_expl = pe_t && (now - pe_t) < HUD_PSD_LEGAL_DELAY_MS;
        uint8_t tl = (d.valid & V_ACC) ? d.acc_tempolimit : 0;
        bool sv = g_vze && (d.valid & V_SIGN);              /* знаки VZE выключены в меню — не смотрим */

        SignItem main = { SIGN_HIDE, 0, 0 };
        if (psd_expl)                                        { main.mode = SIGN_LIMIT; main.val = pe_kmh; main.src = 2; }
        if (main.mode == SIGN_HIDE && sv)                    main = vze_eval(d.sign_raw, d.sign_suppress, tl, true);
        if (main.mode == SIGN_HIDE && (P.src == PSD_LIM_LEGAL || P.src == PSD_LIM_ENDED))
                                                             { main.mode = SIGN_LIMIT; main.val = P.kmh; main.src = 2; }
        if (main.mode != SIGN_HIDE) items[n_items++] = main;

        if (sv) {
            SignItem x2 = vze_eval(d.sign_raw2, d.sign_sup2, 0, false);
            SignItem x3 = vze_eval(d.sign_raw3, d.sign_sup3, 0, false);
            if (x2.mode == SIGN_LIMIT && x2.val != main.val) items[n_items++] = x2;
            if (x3.mode == SIGN_LIMIT && x3.val != main.val && x3.val != x2.val) items[n_items++] = x3;
        }
        if ((psd_expl && pe_novt) || (sv && (vze_eval(d.sign_raw2, d.sign_sup2, 0, false).mode == SIGN_NOOVT ||
                                             vze_eval(d.sign_raw3, d.sign_sup3, 0, false).mode == SIGN_NOOVT)))
            if (n_items < 4) items[n_items++] = (SignItem){ SIGN_NOOVT, 0, (psd_expl && pe_novt) ? 2 : 1 };

        /* ограничение для контроля превышения — всегда основной знак */
        lim_kmh_for_speed = (main.mode == SIGN_LIMIT) ? main.val : 0;
    }

    int smode = SIGN_HIDE; uint16_t sval = 0;
#if HUD_PSD_DIAG
    uint8_t diag_src = 0;
#endif
    if (n_items) {
        SignItem it = items[(now / HUD_SIGN_CYCLE_MS) % n_items];
        smode = it.mode; sval = it.val;
#if HUD_PSD_DIAG
        diag_src = it.src;
#endif
    }
    if (smode != SIGN_HIDE) {
        bool warn_ph = HUD_SIGN_WARN_FLASH && d.sign_warn && (now % 500) < 250;
        set_bg_color(sign_obj, warn_ph ? C_SIGN_WARN : 0xffffff);
        set_border_color(sign_obj, (smode == SIGN_LIMIT || smode == SIGN_NOOVT) ? C_SIGN_RED : C_GRAY);
        vis(sign_end_line, smode == SIGN_END);
        vis(sign_car_l, smode == SIGN_NOOVT);
        vis(sign_car_r, smode == SIGN_NOOVT);
        if (smode == SIGN_END || smode == SIGN_NOOVT) {
            set_text(sign_lbl, "");
        } else {
            snprintf(buf, sizeof buf, "%u", sval);
            set_text(sign_lbl, buf);
            if (smode == SIGN_RAW) {
                set_font(sign_lbl, LV_FONT_DEFAULT);
                set_text_color(sign_lbl, C_GRAY);
            } else {
                set_font(sign_lbl, sval >= 100 ? &hud_font_sign3 : &hud_font_sign2);
                set_text_color(sign_lbl, 0x000000);
            }
            lv_obj_center(sign_lbl);
        }
    }
    vis(sign_obj, smode != SIGN_HIDE);
#if HUD_CAMS
    cams_update(now, &d, &lim_kmh_for_speed);
#endif
#if HUD_PSD_DIAG
    diag_update(now, diag_src);
#endif

    /* --- превышение: красная обводка цифр скорости ---
       превышение = скорость - ограничение; от HUD_OVERSPEED_START % допуска обводка
       проявляется, к 100 % допуска — полностью красная */
    {
        int opa = 0;
        if (lim_kmh_for_speed && (d.valid & V_SPEED) && !lv_obj_has_flag(speed_lbl, LV_OBJ_FLAG_HIDDEN)) {
            float ex = (float)d.speed_kmh - (float)lim_kmh_for_speed;
            float tol = g_tol > 0 ? (float)g_tol : 0.001f;                /* допуск 0 — красная при любом превышении */
            float r  = ex / tol;                                          /* доля допуска */
            float st = HUD_OVERSPEED_START / 100.0f;
            float k  = (r - st) / (1.0f - st);
            if (k > 1) k = 1;
            if (k > 0) opa = (int)(k * 255);
        }
        ov_opa = opa;
        const char *txt_speed = lv_label_get_text(speed_lbl);
        for (int i = 0; i < 8; i++) {
            if (opa > 0) {
                set_text(spd_outline[i], txt_speed);
                if (lv_obj_get_style_text_opa(spd_outline[i], LV_PART_MAIN) != opa)
                    lv_obj_set_style_text_opa(spd_outline[i], (lv_opa_t)opa, 0);
            }
            vis(spd_outline[i], opa > 0);
        }
    }

#if HUD_TSR_STATE
    tsr_update(&d, g_vze);
#endif

    /* значок источника данных — в верхней строке между «сколько заправить» и передачей, на месте кружка TSR
       (кружок показан — значка нет). Нет связи — серый «BT ...», BLE — синий, свой CAN — зелёная надпись CAN */
    {
        int src = d.link_up ? hud_src_current() : -1;
        if (src == 1)      { set_text(link_lbl, "CAN");               set_text_color(link_lbl, C_LINK_CAN); }
        else if (src == 0) { set_text(link_lbl, LV_SYMBOL_BLUETOOTH); set_text_color(link_lbl, C_LINK_BLE); }
        else               { set_text(link_lbl, LV_SYMBOL_BLUETOOTH " ..."); set_text_color(link_lbl, 0x666666); }
        bool lshow = src >= 0 && !link_pf && !tsr_shown;   /* без связи — заставка */
        if (lshow) {
            lv_obj_update_layout(link_lbl);
            int x = top_gap_cx() - lv_obj_get_width(link_lbl) / 2;
            if (lv_obj_get_x(link_lbl) != x) lv_obj_set_x(link_lbl, x);
        }
        vis(link_lbl, lshow);
    }

    /* --- мигалки --- */
    bool bv  = d.valid & V_BLINK;
    bool haz = bv && (d.blink & BLINK_HAZARD);
#if HUD_BLINK_FROM_TAKT
    bool show_l = bv && (d.blink & BLINK_L_TAKT);
    bool show_r = bv && (d.blink & BLINK_R_TAKT);
#else
    static uint32_t seen_l, seen_r;
    if (bv && (d.blink & BLINK_L_TAKT)) seen_l = now ? now : 1;
    if (bv && (d.blink & BLINK_R_TAKT)) seen_r = now ? now : 1;
    bool act_l = haz || (bv && seen_l && now - seen_l < 800);
    bool act_r = haz || (bv && seen_r && now - seen_r < 800);
    bool phase = (now % 1000) < 500;
    bool show_l = act_l && phase;
    bool show_r = act_r && phase;
#endif
    uint32_t bcol = haz ? C_BLINK_RED : C_BLINK_GRN;
    set_img_color(turn_left_sym, bcol);
    set_img_color(turn_right_sym, bcol);
    vis(turn_left_container, show_l);
    vis(turn_right_container, show_r);

    /* --- режим КП: код 0 (промежуточное) не показываем, держим прежний --- */
    static const char *gear_last = NULL;
    static uint8_t gear_code_last = 0;
    if (d.valid & V_GEAR) {
        const char *g = gear_text(d.gear);
        if (g) { gear_last = g; gear_code_last = d.gear; }
        else if (d.gear != 0) { gear_last = NULL; gear_code_last = 0; }
    } else {
        gear_last = NULL; gear_code_last = 0;
    }
    if (gear_last) {
        /* номер передачи — только в D / S / M */
        bool with_num = (d.valid & V_GNUM) && d.gear_num >= 1 && d.gear_num <= 9 &&
                        (gear_code_last == 4 || gear_code_last == 5 || gear_code_last == 6);
        if (with_num) snprintf(buf, sizeof buf, "%s%u", gear_last, d.gear_num);
        else          snprintf(buf, sizeof buf, "%s", gear_last);
        set_text(gear_lbl, buf);
    }
    vis(gear_lbl, gear_last != NULL);

    /* --- сколько заправить до полного бака: объём бака x (100 - % из BAP_BC fct 0x1C) --- */
    {
        bool ok = d.valid & V_BCFUEL;
        if (ok) {
            float l = g_tank * (100u - d.bc_fuel_pct) / 100.0f;
            if (g_gal) {
                unsigned g10 = (unsigned)(l / 3.78541f * 10.0f + 0.5f);       /* галлоны США */
                snprintf(buf, sizeof buf, "%u.%u gal", g10 / 10, g10 % 10);
            } else {
                snprintf(buf, sizeof buf, "%u %s", (unsigned)(l + 0.5f), g_lang == HUD_LANG_EN ? "L" : "л");
            }
            set_text(tank_lbl, buf);
        }
        vis(tank_icon, ok);
        vis(tank_lbl, ok);
    }

    /* --- pre sense: главный приоритет в области стрелки ---
       AWV_Warnung: 2 предупреждение — горит; 3 острое, 4 торможение,
       5 перехватите управление, 6 при повороте — мигает 4 раза в секунду */
    bool ps = (d.valid & V_AWV) && d.awv_warn >= 2 && d.awv_warn <= 6;
    if (ps) {
        bool hot = d.awv_warn >= 3 && (now % 250) < 125;
        set_img_color(ps_tri, hot ? 0x5a0000 : 0xe01414);
    }
    vis(ps_box, ps);

    /* --- двери: машинка на месте стрелки, приоритет над навигацией --- */
    uint8_t doors = 0;
    if (d.valid & V_DOORS) doors |= d.doors & (DOOR_FL | DOOR_FR | DOOR_RL | DOOR_RR | DOOR_TRUNK);
    if (d.valid & V_HOOD)  doors |= d.doors & DOOR_HOOD;
    for (int i = 0; i < 6; i++) {
        vis(car_under[i], doors & (1u << i));
        vis(car_part[i],  doors & (1u << i));
    }
    vis(car_box, doors != 0 && !ps);

    /* --- навигация --- */
    bool nav = (d.valid & V_MAN) && d.rg_active == 1 && doors == 0 && !ps;

    const NavImg *ni = nav ? nav_pick(&nav_big, d.man_main, d.man_dir) : NULL;

    /* следующий манёвр (Maneuver_2) — маленькой серой стрелкой в правом нижнем углу */
    {
        uint8_t m2 = d.man2_main;
        bool show2 = nav && ni != NULL && m2 != 0x00 && m2 != 0x01 && m2 != 0x09 && m2 != 0x0A;
        const NavImg *n2 = show2 ? nav_pick(&nav_small, m2, d.man2_dir) : NULL;
        static const NavImg *n2_last = NULL;
        if (n2 && n2 != n2_last) {
            lv_img_set_src(next_img, n2->img);
            lv_obj_set_pos(next_img, NAV_AREA_W - NAV_SMALL_W - 2 + n2->x, NAV_BOTTOM_Y - NAV_SMALL_H + n2->y);
        }
        n2_last = n2;
        vis(next_img, n2 != NULL);
    }
    vis(arrow_ph, nav);
    bool anim_on = false;       /* true — вместо статичной стрелки показан кадр анимации из /arrows.bin (бар-граф тогда не рисуем) */
    if (nav && ni != NULL && g_anim_on && hud_anim_ready()) {
        static uint8_t am = 0xFF, ad = 0xFF; static int aidx = -1;
        if (d.man_main != am || d.man_dir != ad) {
            am = d.man_main; ad = d.man_dir;
            char nm[40];
            aidx = anim_name(nm, sizeof nm, am, ad) ? hud_anim_find(nm) : -1;
        }
        if (aidx >= 0) {
            anim_on = anim_step(aidx, now);
        }
        g_anim_run = anim_on ? aidx : -1;
    } else {
        g_anim_run = -1;
    }
    vis(anim_img, anim_on);
    if (nav) {
        static const NavImg *ni_last = NULL;
        if (ni && ni != ni_last) {
            lv_img_set_src(arrow_img, ni->img);
            lv_obj_set_pos(arrow_img, ni->x, ni->y + NAV_DY);
        }
        ni_last = ni;
        bool calc = (d.man_main == 0x09 || d.man_main == 0x0A);
        vis(arrow_img, ni != NULL && !anim_on);
        vis(arrow_lbl, ni == NULL && calc);             /* расчёт маршрута; 0x00/0x01 — пусто */

#if HUD_LOG_BAP
        /* журнал манёвров: каждая смена описания — строка в Serial (для таблицы кодов) */
        /* сравниваем всё сообщение 0x17 целиком: манёвров в нём до трёх */
        static uint8_t lg_raw[32], lg_len = 0xFF;
        if (d.man_raw_len != lg_len || memcmp(d.man_raw, lg_raw, sizeof lg_raw) != 0) {
            lg_len = d.man_raw_len;
            memcpy(lg_raw, d.man_raw, sizeof lg_raw);
            char hex[3 * 32 + 1] = ""; int o = 0;
            for (int k = 0; k < d.man_raw_len && k < 32; k++)
                o += snprintf(hex + o, sizeof hex - o, "%02X ", d.man_raw[k]);
            char dist[16] = "-";
            if (d.valid & V_MDIST) fmt_dist(dist, sizeof dist, d.man_dist_x10, d.man_unit);
            arduino_printf("[nav] манёвр main 0x%02X dir 0x%02X (%u°) z 0x%02X  дист %s  0x17[%u]: %s\n",
                           d.man_main, d.man_dir, (unsigned)(d.man_dir * 360u / 256u), d.man_z,
                           dist, d.man_raw_len, hex);
        }
#endif
        if (d.valid & V_MDIST) {
            char dist[16];
            fmt_dist(dist, sizeof dist, d.man_dist_x10, d.man_unit);
            set_text(nav_dist_lbl, dist);
        }
        vis(nav_dist_lbl, (d.valid & V_MDIST) && ni != NULL);
        (void)calc;
    }

    int filled = -1;   /* -1 = бар скрыт */
    if (nav && (HUD_NAV_BAR_TOP || !anim_on) && (d.valid & V_MDIST) && d.bargraph != 0xFF)
        filled = d.bargraph * 16 / 100;
#if HUD_NAV_BAR_TOP
    vis(top_bar, filled >= 0);
    if (filled >= 0) {
        int pct = HUD_ANIM_BAR_INVERT ? 100 - d.bargraph : d.bargraph;   /* полоска растёт к манёвру */
        int w = pct * TOP_BAR_W / 100;
        if (w < 0) w = 0;
        if (lv_obj_get_width(top_fill) != w) lv_obj_set_width(top_fill, w < 1 ? 1 : w);
        vis(top_fill, w > 0);
    }
    for (int i = 0; i < 16; i++) vis(bar_seg[i], false);
#else
    for (int i = 0; i < 16; i++) {
        vis(bar_seg[i], filled >= 0);
        if (filled >= 0) set_bg_color(bar_seg[i], i < filled ? C_BAR_ON : C_BAR_OFF);
    }
#endif

    bool dv = nav && (d.valid & V_DEST);
    bool tv = nav && (d.valid & V_TTE);
    if (dv || tv) {
        char l1[16] = "", l2[16] = "";
        if (dv) fmt_dist(l1, sizeof l1, d.dest_dist_x10, d.dest_unit);
        if (tv) {
            if (d.tte_type == 0) snprintf(l2, sizeof l2, "%u:%02u%s", d.tte_h, d.tte_min, txt()->h);
            else                 snprintf(l2, sizeof l2, "%02u:%02u", d.tte_h, d.tte_min);
        }
        snprintf(buf, sizeof buf, "%s\n%s", l1, l2);
        set_text(route_lbl, buf);
    }
    vis(route_icon, dv || tv);
    vis(route_lbl, dv || tv);

    if (g_mode == HUD_MODE_MIN) {
        MinCtx mc = {
            .nav = nav, .ni = nav ? nav_pick(&nav_big, d.man_main, d.man_dir) : NULL, .calc = (d.man_main == 0x09 || d.man_main == 0x0A),
            .acc_on = acc_on, .acc_set = acc_set, .acc_status = d.acc_status, .acc_col = acc_col,
            .lim_on = lim_on, .lim_set = lim_set, .lka_on = lka_on, .aca_on = aca_on,
            .obj = obj, .obj_warn = obj_warn, .arcs = arcs, .swa_i = swa_i, .swa_w = swa_w,
            .smode = smode, .sval = sval, .sign_warn = d.sign_warn, .ov_opa = ov_opa,
        };
        mn_update(&d, now, &mc);
    }
}

/* ============================================================================
   Построение экрана (координаты — v6)
   ========================================================================== */
/* Мигалки: внизу, поверх остальных элементов (создаются последними),
   на 5 px выше исходных y=124. Картинка — две маски: чёрная обводка + заливка,
   цвет заливки (зелёный / красный) задаётся img_recolor. */
#define BLINK_L_X  2
#define BLINK_R_X  (640 - 46 - 2)
#define BLINK_Y    119

static lv_obj_t *make_blinker(lv_obj_t *scr, int x, int y,
                              const lv_img_dsc_t *outline, const lv_img_dsc_t *fill,
                              lv_obj_t **fill_out)
{
    lv_obj_t *c = lv_obj_create(scr);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, outline->header.w, outline->header.h);
    lv_obj_set_pos(c, x, y);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *o = lv_img_create(c);
    lv_img_set_src(o, outline);
    lv_obj_set_style_img_recolor(o, lv_color_black(), 0);
    lv_obj_set_style_img_recolor_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_pos(o, 0, 0);

    lv_obj_t *f = lv_img_create(c);
    lv_img_set_src(f, fill);
    lv_obj_set_style_img_recolor(f, lv_color_hex(C_BLINK_GRN), 0);
    lv_obj_set_style_img_recolor_opa(f, LV_OPA_COVER, 0);
    lv_obj_set_pos(f, 0, 0);

    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    *fill_out = f;
    return c;
}

static lv_obj_t *car_img(lv_obj_t *parent, const lv_img_dsc_t *src, int x, int y, uint32_t col)
{
    lv_obj_t *i = lv_img_create(parent);
    lv_img_set_src(i, src);
    lv_obj_set_style_img_recolor(i, lv_color_hex(col), 0);
    lv_obj_set_style_img_recolor_opa(i, LV_OPA_COVER, 0);
    lv_obj_set_pos(i, x, y);
    return i;
}


/* ============================================================================
   Меню настроек (касание экрана)
   Выбор из вариантов: показаны все варианты, активный — зелёный, остальные серые;
   нажатие на серый переключает. Переключатели (VZE, PSD): зелёный — вкл,
   красный — выкл. Звук и допуск превышения — кнопки с текущим значением, по нажатию
   поверх меню открывается панель с ползунком и кнопками - / +.
   Меняется сразу, сохраняется в NVS. Закрывается кнопкой ✕ или само через
   HUD_MENU_TIMEOUT_MS без касаний.
   ========================================================================== */
#if HUD_TOUCH_ENABLE
#define C_MENU_BG     0x101010
#define C_MENU_ON     0x2a8a2a
#define C_MENU_OFFR   0x9a2222
#define C_MENU_BTN    0x3a3a3a
#define C_MENU_DIM    0x9a9a9a

/* кнопки: основной экран (B_SND..B_MORE) и дополнительное меню «Ещё» (B_LANG_RU..B_BACK) */
enum { B_LANG_RU, B_LANG_EN, B_UNIT_KM, B_UNIT_MI, B_VOL_L, B_VOL_G, B_TANK, B_LDR, B_BACK,
       B_SND, B_TOL, B_VZE, B_PSD, B_MODE, B_CAM, B_CBEEP, B_FUEL, B_ANIM, B_MORE, B_COUNT };
enum { G_LANG, G_UNITS, G_VOL, G_TANK, G_LDR, G_COUNT };           /* заголовки колонок доп. меню */
static lv_obj_t *menu_box, *menu_pg[2], *menu_title[G_COUNT], *menu_btn[B_COUNT], *menu_btn_lbl[B_COUNT];
static uint32_t  menu_last_touch;

/* панель ползунка: 0 — громкость, 1 — допуск превышения, 2 — объём бака */
static lv_obj_t *ov_box, *ov_title, *ov_val, *ov_slider, *ov_ok_lbl;
static int       ov_kind;

/* панель калибровки датчика света: «темно» (закрыт) и «светло» (фонарик) */
static lv_obj_t *cal_box, *cal_title, *cal_live, *cal_msg, *cal_lbl_dark, *cal_lbl_bright,
                *cal_lbl_save, *cal_lbl_en, *cal_lbl_cancel;
static int       cal_dark, cal_bright;           /* захваченные, мВ; -1 — ещё нет */

static void menu_close(void);

static void menu_sel(int b, bool active)
{
    lv_obj_set_style_bg_color(menu_btn[b], lv_color_hex(active ? C_MENU_ON : C_MENU_BTN), 0);
    lv_obj_set_style_text_color(menu_btn_lbl[b], lv_color_hex(active ? 0xffffff : C_MENU_DIM), 0);
}

static void menu_sw(int b, bool on)
{
    lv_obj_set_style_bg_color(menu_btn[b], lv_color_hex(on ? C_MENU_ON : C_MENU_OFFR), 0);
    lv_obj_set_style_text_color(menu_btn_lbl[b], lv_color_white(), 0);
}

static void menu_page(int p)
{
    for (int i = 0; i < 2; i++) { if (i == p) lv_obj_clear_flag(menu_pg[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(menu_pg[i], LV_OBJ_FLAG_HIDDEN); }
}

static void menu_refresh(void)
{
    bool en = g_lang == HUD_LANG_EN, mi = g_units == HUD_UNITS_MI;
    static const char *t_ru[G_COUNT] = { "Язык", "Единицы", "Объём", "Бак", "Яркость" };
    static const char *t_en[G_COUNT] = { "Language", "Units", "Volume", "Tank", "Brightness" };
    for (int g = 0; g < G_COUNT; g++) lv_label_set_text(menu_title[g], en ? t_en[g] : t_ru[g]);
    char buf[40];

    /* основной экран */
    snprintf(buf, sizeof buf, "%s\n%u", en ? "Sound" : "Звук", (unsigned)hud_sound_get_volume());
    lv_label_set_text(menu_btn_lbl[B_SND], buf);
    snprintf(buf, sizeof buf, "%s\n%u %s", en ? "Margin" : "Допуск", (unsigned)g_tol, en ? "km/h" : "км/ч");
    lv_label_set_text(menu_btn_lbl[B_TOL], buf);
    lv_label_set_text(menu_btn_lbl[B_VZE], en ? "Road\nsigns" : "Знаки на\nдороге");
    lv_label_set_text(menu_btn_lbl[B_PSD], en ? "Speed limit\non the road" : "Ограничение\nна дороге");
    snprintf(buf, sizeof buf, "%s\n%s", en ? "Display" : "Режим", g_mode == HUD_MODE_MIN ? (en ? "Minimal" : "Минимум") : (en ? "Full" : "Полный"));
    lv_label_set_text(menu_btn_lbl[B_MODE], buf);
    lv_label_set_text(menu_btn_lbl[B_CAM],   en ? "Cameras" : "Камеры");
    lv_label_set_text(menu_btn_lbl[B_CBEEP], en ? "Camera\nalert" : "Сигнал\nкамер");
    snprintf(buf, sizeof buf, "%s\n%s", en ? "Fuel" : "Расход",
             g_fmode == 0 ? (en ? "Instant" : "Мгнов.") : g_fmode == 1 ? (en ? "Average" : "Средний") : (en ? "Off" : "Выкл"));
    lv_label_set_text(menu_btn_lbl[B_FUEL], buf);
    lv_label_set_text(menu_btn_lbl[B_ANIM], en ? "Arrow\nanimation" : "Анимация\nстрелок");
    {   /* нет /arrows.bin — пункта анимации нет, «Расход» занимает его место */
        bool ar = hud_anim_ready();
        if (ar) lv_obj_clear_flag(menu_btn[B_ANIM], LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(menu_btn[B_ANIM], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(menu_btn[B_FUEL], ar ? 125 : 125 + 3 + 172);
    }
    lv_label_set_text(menu_btn_lbl[B_MORE], en ? "More >" : "Ещё >");
    menu_sel(B_SND, false);  menu_sel(B_TOL, false);  menu_sel(B_FUEL, false);  menu_sel(B_MORE, false);  menu_sel(B_MODE, false);
    menu_sw(B_VZE, g_vze);   menu_sw(B_PSD, g_psd);
    menu_sw(B_CAM, g_cams_on);  menu_sw(B_CBEEP, g_cam_beep);  menu_sw(B_ANIM, g_anim_on);

    /* дополнительное меню */
    lv_label_set_text(menu_btn_lbl[B_LANG_RU], "Русский");
    lv_label_set_text(menu_btn_lbl[B_LANG_EN], "English");
    lv_label_set_text(menu_btn_lbl[B_UNIT_KM], en ? "km" : "км");
    lv_label_set_text(menu_btn_lbl[B_UNIT_MI], en ? "miles" : "мили");
    lv_label_set_text(menu_btn_lbl[B_VOL_L],   en ? "liters" : "литры");
    lv_label_set_text(menu_btn_lbl[B_VOL_G],   en ? "gallons" : "галлоны");
    if (g_gal) { unsigned g10 = (unsigned)(g_tank / 3.78541f * 10.0f + 0.5f); snprintf(buf, sizeof buf, "%u.%u gal", g10 / 10, g10 % 10); }
    else       snprintf(buf, sizeof buf, "%u %s", (unsigned)g_tank, en ? "L" : "л");
    lv_label_set_text(menu_btn_lbl[B_TANK], buf);
    lv_label_set_text(menu_btn_lbl[B_LDR],  en ? "Calibrate\nsensor" : "Калибровка\nдатчика");
    lv_label_set_text(menu_btn_lbl[B_BACK], en ? "< Back" : "< Назад");
    menu_sel(B_LANG_RU, !en);        menu_sel(B_LANG_EN, en);
    menu_sel(B_UNIT_KM, !mi);        menu_sel(B_UNIT_MI, mi);
    menu_sel(B_VOL_L,   !g_gal);     menu_sel(B_VOL_G,   g_gal);
    menu_sel(B_TANK, false);         menu_sel(B_BACK, false);
    menu_sel(B_LDR, hud_light_enabled());
    {   /* кнопки-действия и кнопки со значением: текст всегда белый (серым показывается только неактивный вариант) */
        static const int act[] = { B_SND, B_TOL, B_FUEL, B_MODE, B_MORE, B_TANK, B_LDR, B_BACK };
        for (unsigned i = 0; i < sizeof act / sizeof act[0]; i++)
            lv_obj_set_style_text_color(menu_btn_lbl[act[i]], lv_color_white(), 0);
    }
}

/* ---- панель ползунка ---- */
static int ov_lo(void)  { return ov_kind == 2 ? 30 : 0; }
static int ov_hi(void)  { return ov_kind == 0 ? 100 : ov_kind == 1 ? 20 : 90; }
static int ov_cur(void) { return ov_kind == 0 ? (int)hud_sound_get_volume() : ov_kind == 1 ? (int)g_tol : (int)g_tank; }

static void ov_labels(void)
{
    bool en = g_lang == HUD_LANG_EN;
    int  v  = ov_cur();
    char buf[48];
    lv_label_set_text(ov_title, ov_kind == 0 ? (en ? "Sound volume" : "Громкость звука")
                              : ov_kind == 1 ? (en ? "Overspeed margin, km/h" : "Допуск превышения, км/ч")
                                             : (en ? "Fuel tank volume, L" : "Объём бака, л"));
    snprintf(buf, sizeof buf, "%d", v);
    lv_label_set_text(ov_val, buf);
    lv_label_set_text(ov_ok_lbl, en ? "OK" : "Готово");
#if LV_USE_SLIDER
    lv_slider_set_value(ov_slider, v, LV_ANIM_OFF);
#endif
}

static void ov_set(int v)
{
    if (v < ov_lo()) v = ov_lo();
    if (v > ov_hi()) v = ov_hi();
    if (ov_kind == 0)      hud_sound_set_volume((uint8_t)v);
    else if (ov_kind == 1) hud_set_overspeed_tol((uint8_t)v);
    else                   hud_set_tank((uint8_t)v);
    ov_labels();
    menu_refresh();
}

static void ov_beep(void)
{
    if (ov_kind == 0) hud_sound_play(hud_snd_swa_beep, hud_snd_swa_beep_len);   /* на слух выбрать громкость */
}

static void ov_open(int kind)
{
    ov_kind = kind;
    ov_labels();
#if LV_USE_SLIDER
    lv_slider_set_range(ov_slider, ov_lo(), ov_hi());
    lv_slider_set_value(ov_slider, ov_cur(), LV_ANIM_OFF);
#endif
    lv_obj_clear_flag(ov_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ov_box);
}

static void ov_close(void)
{
    if (lv_obj_has_flag(ov_box, LV_OBJ_FLAG_HIDDEN)) return;
    lv_obj_add_flag(ov_box, LV_OBJ_FLAG_HIDDEN);
    hud_settings_save();
}

static void menu_any_touch_cb(lv_event_t *e) { (void)e; menu_last_touch = hud_now_ms(); }

/* ---- панель калибровки датчика света ---- */
static void cal_text(void)
{
    bool en = g_lang == HUD_LANG_EN;
    char buf[96], a[16], b[16];
    int mv = hud_light_mv();
    if (cal_dark   >= 0) snprintf(a, sizeof a, "%d", cal_dark);   else snprintf(a, sizeof a, "-");
    if (cal_bright >= 0) snprintf(b, sizeof b, "%d", cal_bright); else snprintf(b, sizeof b, "-");
    snprintf(buf, sizeof buf, en ? "Now: %d mV   Dark: %s   Bright: %s" : "Сейчас: %d мВ   Темно: %s   Светло: %s",
             mv < 0 ? 0 : mv, a, b);
    lv_label_set_text(cal_live, buf);
}

static void cal_labels(void)
{
    bool en = g_lang == HUD_LANG_EN;
    lv_label_set_text(cal_title, en ? "Light sensor calibration" : "Калибровка датчика света");
    lv_label_set_text(cal_lbl_dark,   en ? "1. Cover the sensor\nand tap" : "1. Закройте датчик\nи нажмите");
    lv_label_set_text(cal_lbl_bright, en ? "2. Shine a torch\nand tap"    : "2. Посветите фонариком\nи нажмите");
    lv_label_set_text(cal_lbl_save,   en ? "Save" : "Сохранить");
    lv_label_set_text(cal_lbl_en,     hud_light_enabled() ? (en ? "Sensor: ON" : "Датчик: ВКЛ")
                                                          : (en ? "Sensor: OFF" : "Датчик: ВЫКЛ"));
    lv_label_set_text(cal_lbl_cancel, en ? "Cancel" : "Отмена");
    cal_text();
}

static void cal_open(void)
{
    cal_dark = cal_bright = -1;
    lv_label_set_text(cal_msg, "");
    hud_light_force(true);
    cal_labels();
    lv_obj_clear_flag(cal_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(cal_box);
}

static void cal_close(void)
{
    if (lv_obj_has_flag(cal_box, LV_OBJ_FLAG_HIDDEN)) return;
    lv_obj_add_flag(cal_box, LV_OBJ_FLAG_HIDDEN);
    hud_light_force(false);
}

static void cal_btn_cb(lv_event_t *e)
{
    bool en = g_lang == HUD_LANG_EN;
    int  k  = (int)(intptr_t)lv_event_get_user_data(e);
    int  mv = hud_light_mv();
    menu_last_touch = hud_now_ms();
    switch (k) {
    case 0: case 1:                                   /* захватить «темно» / «светло» */
        if (mv < 0) { lv_label_set_text(cal_msg, en ? "No reading yet" : "Нет измерения"); break; }
        if (k == 0) cal_dark = mv; else cal_bright = mv;
        lv_label_set_text(cal_msg, "");
        break;
    case 2:                                           /* сохранить */
        if (cal_dark < 0 || cal_bright < 0) {
            lv_label_set_text(cal_msg, en ? "Do both steps first" : "Сначала оба шага");
        } else if (!hud_light_cal_valid(cal_dark, cal_bright)) {
            lv_label_set_text(cal_msg, en ? "Too little difference - repeat" : "Слишком мала разница - повторите");
        } else {
            hud_light_set_cal(cal_dark, cal_bright);
            hud_light_set_enabled(true);
            hud_settings_save();
            cal_close();
            menu_refresh();
            return;
        }
        break;
    case 3:                                           /* датчик вкл / выкл */
        hud_light_set_enabled(!hud_light_enabled());
        hud_settings_save();
        menu_refresh();
        break;
    case 4:                                           /* отмена */
        cal_close();
        return;
    }
    cal_labels();
}

#if LV_USE_SLIDER
static void ov_slider_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_VALUE_CHANGED) ov_set(lv_slider_get_value(ov_slider));
    else if (c == LV_EVENT_RELEASED) ov_beep();
    menu_last_touch = hud_now_ms();
}
#endif
static void ov_step_cb(lv_event_t *e)
{
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    int step = ov_kind == 0 ? 5 : 1;
    ov_set(ov_cur() + dir * step);
    ov_beep();
    menu_last_touch = hud_now_ms();
}
static void ov_ok_cb(lv_event_t *e) { (void)e; ov_close(); menu_last_touch = hud_now_ms(); }

static void menu_btn_cb(lv_event_t *e)
{
    switch ((int)(intptr_t)lv_event_get_user_data(e)) {
    case B_LANG_RU: hud_set_lang(HUD_LANG_RU); break;
    case B_LANG_EN: hud_set_lang(HUD_LANG_EN); break;
    case B_UNIT_KM: hud_set_units(HUD_UNITS_KM); break;
    case B_UNIT_MI: hud_set_units(HUD_UNITS_MI); break;
    case B_VOL_L:   hud_set_gallons(false); break;
    case B_VOL_G:   hud_set_gallons(true); break;
    case B_FUEL:    hud_set_fuel_mode((g_fmode + 1) % 3); break;           /* мгновенный -> средний -> выкл */
    case B_VZE:     hud_set_vze(!g_vze); break;
    case B_PSD:     hud_set_psd(!g_psd); break;
    case B_ANIM:    hud_set_anim(!g_anim_on); break;
    case B_MODE:    hud_set_mode(g_mode == HUD_MODE_MIN ? HUD_MODE_FULL : HUD_MODE_MIN); break;   /* полный <-> минимальный */
    case B_CAM:     hud_set_cams(!g_cams_on); break;
    case B_CBEEP:   hud_set_cam_beep(!g_cam_beep); break;
    case B_SND:     menu_last_touch = hud_now_ms(); ov_open(0); return;
    case B_TOL:     menu_last_touch = hud_now_ms(); ov_open(1); return;
    case B_TANK:    menu_last_touch = hud_now_ms(); ov_open(2); return;
    case B_LDR:     menu_last_touch = hud_now_ms(); cal_open(); return;
    case B_MORE:    menu_last_touch = hud_now_ms(); menu_page(1); return;
    case B_BACK:    menu_last_touch = hud_now_ms(); menu_page(0); return;
    }
    menu_last_touch = hud_now_ms();
    menu_refresh();
    hud_settings_save();
}

static void menu_close(void) { ov_close(); cal_close(); lv_obj_add_flag(menu_box, LV_OBJ_FLAG_HIDDEN); }

/* запрос меню от задачи тача (одиночное / двойное касание, HUD_MENU_DOUBLE_TAP) */
extern bool hud_touch_take_menu_request(void);

static void menu_open_cb(lv_event_t *e)
{
    (void)e;
    menu_refresh();
    menu_page(0);
    menu_last_touch = hud_now_ms();
    lv_obj_add_flag(ov_box, LV_OBJ_FLAG_HIDDEN);
    cal_close();
    lv_obj_clear_flag(menu_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(menu_box);
}

static void menu_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (hud_ota_mode()) return;                            /* режим обновления: меню не нужно */
    bool req = hud_touch_take_menu_request();
    /* безопасность: меню только на стоящей машине (скорость по CAN <= HUD_MENU_MAX_KMH);
       нет скорости вовсе (стенд, нет связи) — тоже можно */
    bool allowed = true;
#if HUD_MENU_ONLY_STOPPED
    HudData d;
    hud_data_snapshot(&d);
    if ((d.valid & V_SPEED) && d.speed_kmh > HUD_MENU_MAX_KMH) allowed = false;
#endif
    if (lv_obj_has_flag(menu_box, LV_OBJ_FLAG_HIDDEN)) {
        if (req && allowed) menu_open_cb(NULL);            /* открыть по касанию */
    } else if (req && lv_obj_has_flag(ov_box, LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(cal_box, LV_OBJ_FLAG_HIDDEN)) {
        menu_close();                                      /* двойное касание где угодно — закрыть */
    } else if (!allowed || hud_now_ms() - menu_last_touch >
               (lv_obj_has_flag(cal_box, LV_OBJ_FLAG_HIDDEN) ? HUD_MENU_TIMEOUT_MS : 60000)) {
        menu_close();                                      /* тронулись или нет касаний HUD_MENU_TIMEOUT_MS */
    }
    if (!lv_obj_has_flag(cal_box, LV_OBJ_FLAG_HIDDEN)) cal_text();      /* живое значение датчика */
}

static lv_obj_t *menu_mk_btn(lv_obj_t *parent, int x, int y, int w, int h, lv_obj_t **lbl_out)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_MENU_BTN), 0);
    lv_obj_add_event_cb(b, menu_any_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &hud_font_menu, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, "");
    lv_obj_center(l);
    *lbl_out = l;
    return b;
}

static void build_ov(lv_obj_t *scr)
{
    ov_box = lv_obj_create(scr);
    lv_obj_remove_style_all(ov_box);
    lv_obj_set_size(ov_box, 640, 172);
    lv_obj_set_pos(ov_box, 0, 0);
    lv_obj_set_style_bg_color(ov_box, lv_color_hex(C_MENU_BG), 0);
    lv_obj_set_style_bg_opa(ov_box, LV_OPA_COVER, 0);
    lv_obj_add_flag(ov_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(ov_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ov_box, menu_any_touch_cb, LV_EVENT_PRESSED, NULL);

    ov_title = lv_label_create(ov_box);
    lv_obj_set_style_text_font(ov_title, &hud_font_menu, 0);
    lv_obj_set_style_text_color(ov_title, lv_color_hex(C_MENU_DIM), 0);
    lv_obj_set_width(ov_title, 640);
    lv_obj_set_style_text_align(ov_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(ov_title, 0, 8);
    ov_val = lv_label_create(ov_box);
    lv_obj_set_style_text_font(ov_val, &hud_font_menu, 0);
    lv_obj_set_style_text_color(ov_val, lv_color_white(), 0);
    lv_obj_set_width(ov_val, 640);
    lv_obj_set_style_text_align(ov_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(ov_val, 0, 36);

    lv_obj_t *l;
    lv_obj_t *bm = menu_mk_btn(ov_box, 14, 68, 80, 56, &l);
    lv_label_set_text(l, "-");
    lv_obj_add_event_cb(bm, ov_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    lv_obj_t *bp = menu_mk_btn(ov_box, 546, 68, 80, 56, &l);
    lv_label_set_text(l, "+");
    lv_obj_add_event_cb(bp, ov_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
#if LV_USE_SLIDER
    ov_slider = lv_slider_create(ov_box);
    lv_obj_set_size(ov_slider, 410, 16);
    lv_obj_set_pos(ov_slider, 115, 88);
    lv_slider_set_range(ov_slider, 0, 100);
    lv_obj_set_style_bg_color(ov_slider, lv_color_hex(0x3a3a3a), LV_PART_MAIN);
    lv_obj_set_style_bg_color(ov_slider, lv_color_hex(C_MENU_ON), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ov_slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(ov_slider, 10, LV_PART_KNOB);                 /* ручка покрупнее под палец */
    lv_obj_add_event_cb(ov_slider, ov_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(ov_slider, ov_slider_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(ov_slider, menu_any_touch_cb, LV_EVENT_PRESSING, NULL);
#endif
    lv_obj_t *ok = menu_mk_btn(ov_box, 220, 130, 200, 36, &ov_ok_lbl);
    lv_obj_set_style_bg_color(ok, lv_color_hex(C_MENU_ON), 0);
    lv_obj_add_event_cb(ok, ov_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(ov_box, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *cal_mk_label(lv_obj_t *parent, int y, bool dim)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, &hud_font_menu, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(dim ? C_MENU_DIM : 0xffffff), 0);
    lv_obj_set_width(l, 640);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(l, 0, y);
    return l;
}

static void build_cal(lv_obj_t *scr)
{
    cal_box = lv_obj_create(scr);
    lv_obj_remove_style_all(cal_box);
    lv_obj_set_size(cal_box, 640, 172);
    lv_obj_set_pos(cal_box, 0, 0);
    lv_obj_set_style_bg_color(cal_box, lv_color_hex(C_MENU_BG), 0);
    lv_obj_set_style_bg_opa(cal_box, LV_OPA_COVER, 0);
    lv_obj_add_flag(cal_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(cal_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(cal_box, menu_any_touch_cb, LV_EVENT_PRESSED, NULL);

    cal_title = cal_mk_label(cal_box, 4, true);
    cal_live  = cal_mk_label(cal_box, 28, false);
    cal_msg   = cal_mk_label(cal_box, 108, false);
    lv_obj_set_style_text_color(cal_msg, lv_color_hex(0xe0a020), 0);

    lv_obj_t *b;
    b = menu_mk_btn(cal_box, 14, 54, 300, 52, &cal_lbl_dark);
    lv_obj_add_event_cb(b, cal_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)0);
    b = menu_mk_btn(cal_box, 326, 54, 300, 52, &cal_lbl_bright);
    lv_obj_add_event_cb(b, cal_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    b = menu_mk_btn(cal_box, 14, 132, 190, 34, &cal_lbl_save);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_MENU_ON), 0);
    lv_obj_add_event_cb(b, cal_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)2);
    b = menu_mk_btn(cal_box, 225, 132, 190, 34, &cal_lbl_en);
    lv_obj_add_event_cb(b, cal_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)3);
    b = menu_mk_btn(cal_box, 436, 132, 190, 34, &cal_lbl_cancel);
    lv_obj_add_event_cb(b, cal_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)4);
    lv_obj_add_flag(cal_box, LV_OBJ_FLAG_HIDDEN);
}

static void build_menu(lv_obj_t *scr)
{
    /* ловушка касаний поверх экрана HUD: короткое касание -> меню */
    lv_obj_t *catcher = lv_obj_create(scr);
    lv_obj_remove_style_all(catcher);
    lv_obj_set_size(catcher, 640, 172);
    lv_obj_set_pos(catcher, 0, 0);
    lv_obj_add_flag(catcher, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(catcher, LV_OBJ_FLAG_SCROLLABLE);
    /* касания для меню распознаются в задаче тача (lvgl_port.c), см. menu_timer_cb */

    menu_box = lv_obj_create(scr);
    lv_obj_remove_style_all(menu_box);
    lv_obj_set_size(menu_box, 640, 172);
    lv_obj_set_pos(menu_box, 0, 0);
    lv_obj_set_style_bg_color(menu_box, lv_color_hex(C_MENU_BG), 0);
    lv_obj_set_style_bg_opa(menu_box, LV_OPA_COVER, 0);
    lv_obj_add_flag(menu_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(menu_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(menu_box, menu_any_touch_cb, LV_EVENT_PRESSED, NULL);

    for (int pg = 0; pg < 2; pg++) {                       /* страницы: 0 — основной экран, 1 — «Ещё» */
        menu_pg[pg] = lv_obj_create(menu_box);
        lv_obj_remove_style_all(menu_pg[pg]);
        lv_obj_set_size(menu_pg[pg], 640, 172);
        lv_obj_set_pos(menu_pg[pg], 0, 0);
        lv_obj_clear_flag(menu_pg[pg], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(menu_pg[pg], menu_any_touch_cb, LV_EVENT_PRESSED, NULL);
    }
    #define MKB(id, pg, x, y, w, h) menu_btn[id] = menu_mk_btn(menu_pg[pg], x, y, w, h, &menu_btn_lbl[id])

    /* основной экран: 2 ряда по 80 px; ряд 1 — звук, допуск, знаки, ограничение, режим отображения;
       ряд 2 — камеры, сигнал камер, расход (на две колонки), «Ещё». Колонки ряда 2 — под колонками ряда 1 */
    {
        const int g = 3, x0 = 3, y1 = 4, y2 = 88, h = 80;
        const int w1[5] = { 100, 110, 125, 172, 113 };
        int x[5]; x[0] = x0; for (int i = 1; i < 5; i++) x[i] = x[i - 1] + w1[i - 1] + g;
        MKB(B_SND,   0, x[0], y1, w1[0], h);  MKB(B_TOL,  0, x[1], y1, w1[1], h);
        MKB(B_VZE,   0, x[2], y1, w1[2], h);  MKB(B_PSD,  0, x[3], y1, w1[3], h);
        MKB(B_MODE,  0, x[4], y1, w1[4], h);
        MKB(B_CAM,   0, x[0], y2, w1[0], h);  MKB(B_CBEEP, 0, x[1], y2, w1[1], h);
        MKB(B_FUEL,  0, x[2], y2, w1[2] + g + w1[3], h);  MKB(B_ANIM, 0, x[3], y2, w1[3], h);
        MKB(B_MORE,  0, x[4], y2, w1[4], h);
#if !HUD_CAMS
        lv_obj_add_flag(menu_btn[B_CAM], LV_OBJ_FLAG_HIDDEN);          /* камеры не скомпилированы */
        lv_obj_add_flag(menu_btn[B_CBEEP], LV_OBJ_FLAG_HIDDEN);
#endif
    }

    /* дополнительное меню «Ещё»: колонки заголовок + кнопки; редко меняемое */
    {
        const int g = 3, ytop = 36, hhalf = 61, hfull = 128;
        const int cx[6] = { 3, 102, 201, 300, 399, 537 };
        const int cw[6] = { 98, 98, 98, 98, 135, 100 };
        for (int k = 0; k < G_COUNT; k++) {
            menu_title[k] = lv_label_create(menu_pg[1]);
            lv_obj_set_style_text_font(menu_title[k], &hud_font_menu, 0);
            lv_obj_set_style_text_color(menu_title[k], lv_color_hex(C_MENU_DIM), 0);
            lv_obj_set_width(menu_title[k], cw[k] + 24);               /* шире колонки: «Единицы» не помещается в 98 px */
            lv_obj_set_style_text_align(menu_title[k], LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(menu_title[k], cx[k] - 12, 7);
        }
        MKB(B_LANG_RU, 1, cx[0], ytop, cw[0], hhalf);  MKB(B_LANG_EN, 1, cx[0], ytop + hhalf + 6, cw[0], hhalf);
        MKB(B_UNIT_KM, 1, cx[1], ytop, cw[1], hhalf);  MKB(B_UNIT_MI, 1, cx[1], ytop + hhalf + 6, cw[1], hhalf);
        MKB(B_VOL_L,   1, cx[2], ytop, cw[2], hhalf);  MKB(B_VOL_G,   1, cx[2], ytop + hhalf + 6, cw[2], hhalf);
        MKB(B_TANK,    1, cx[3], ytop, cw[3], hfull);
        MKB(B_LDR,     1, cx[4], ytop, cw[4], hfull);
        MKB(B_BACK,    1, cx[5], ytop, cw[5], hfull);
        (void)g;
    }
    #undef MKB
    for (int b = 0; b < B_COUNT; b++)
        lv_obj_add_event_cb(menu_btn[b], menu_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)b);
    menu_page(0);

    build_ov(scr);                         /* панель ползунка — поверх меню */
    build_cal(scr);                        /* панель калибровки датчика света */
    menu_refresh();
    lv_obj_add_flag(menu_box, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(menu_timer_cb, 50, NULL);
}
#endif

/* ---------- экран режима обновления (OTA) ---------- */
static lv_obj_t *ota_box, *ota_l[5], *ota_fg;

static lv_obj_t *ota_label(lv_obj_t *p, int y, uint32_t col)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, &hud_font_menu, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_width(l, 640);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(l, 0, y);
    return l;
}

static void ota_timer_cb(lv_timer_t *t)
{
    (void)t;
    HudOtaInfo i;
    hud_ota_get(&i);
    bool en = g_lang == HUD_LANG_EN;
    char buf[96];
    lv_label_set_text(ota_l[0], en ? "Firmware update" : "Обновление прошивки");
    snprintf(buf, sizeof buf, en ? "Wi-Fi: %s    Password: %s" : "Wi-Fi: %s    Пароль: %s", i.ssid, i.pass);
    lv_label_set_text(ota_l[1], buf);
    snprintf(buf, sizeof buf, en ? "Open in a browser: http://%s" : "Откройте в браузере: http://%s", i.ip);
    lv_label_set_text(ota_l[2], buf);
    int pct = i.state == HUD_OTA_DONE ? 100 : (i.state == HUD_OTA_UPLOAD ? i.percent : 0);
    lv_obj_set_width(ota_fg, pct * 560 / 100);
    switch (i.state) {
    case HUD_OTA_UPLOAD: snprintf(buf, sizeof buf, en ? "Uploading: %d%%" : "Загрузка: %d%%", i.percent); break;
    case HUD_OTA_DONE:   snprintf(buf, sizeof buf, "%s", en ? "Done, rebooting..." : "Готово, перезагрузка..."); break;
    case HUD_OTA_ERROR:  snprintf(buf, sizeof buf, en ? "Error: %s" : "Ошибка: %s", i.msg); break;
    default:             snprintf(buf, sizeof buf, en ? "BOOT - exit, %d:%02d left" : "BOOT - выход, осталось %d:%02d",
                                  i.left_s / 60, i.left_s % 60);
    }
    lv_label_set_text(ota_l[3], buf);
    lv_obj_set_style_text_color(ota_l[3], lv_color_hex(i.state == HUD_OTA_ERROR ? 0xff5050 : 0xc0c0c0), 0);
}

static void build_ota(lv_obj_t *scr)
{
    ota_box = lv_obj_create(scr);
    lv_obj_remove_style_all(ota_box);
    lv_obj_set_size(ota_box, 640, 172);
    lv_obj_set_pos(ota_box, 0, 0);
    lv_obj_set_style_bg_color(ota_box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ota_box, LV_OPA_COVER, 0);
    lv_obj_add_flag(ota_box, LV_OBJ_FLAG_CLICKABLE);               /* глотает касания */
    lv_obj_clear_flag(ota_box, LV_OBJ_FLAG_SCROLLABLE);
    ota_l[0] = ota_label(ota_box, 8, 0xffffff);
    ota_l[1] = ota_label(ota_box, 44, 0x60c0ff);
    ota_l[2] = ota_label(ota_box, 74, 0xffffff);
    lv_obj_t *bg = lv_obj_create(ota_box);
    lv_obj_remove_style_all(bg);
    lv_obj_set_size(bg, 560, 14);
    lv_obj_set_pos(bg, 40, 108);
    lv_obj_set_style_bg_color(bg, lv_color_hex(0x3a3a3a), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
    ota_fg = lv_obj_create(ota_box);
    lv_obj_remove_style_all(ota_fg);
    lv_obj_set_size(ota_fg, 0, 14);
    lv_obj_set_pos(ota_fg, 40, 108);
    lv_obj_set_style_bg_color(ota_fg, lv_color_hex(0x2a8a2a), 0);
    lv_obj_set_style_bg_opa(ota_fg, LV_OPA_COVER, 0);
    ota_l[3] = ota_label(ota_box, 136, 0xc0c0c0);
    lv_timer_create(ota_timer_cb, 100, NULL);
    ota_timer_cb(NULL);
}

void build_hud_mockup(void)
{
    if (HUD_LOG_HUD) arduino_printf("[hud] build_hud_mockup(), free heap=%u\n", (unsigned)esp_get_free_heap_size());

    lv_obj_t *scr0 = lv_scr_act();
    lv_obj_set_style_bg_color(scr0, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr0, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr0, LV_OBJ_FLAG_SCROLLABLE);

    /* Полный режим — всё внутри full_root (в минималистичном режиме скрывается целиком);
       минималистичный — mn_box; поверх обоих: двери, pre sense, мигалки, меню, экран обновления */
    full_root = lv_obj_create(scr0);
    lv_obj_remove_style_all(full_root);
    lv_obj_set_size(full_root, 640, 172);
    lv_obj_set_pos(full_root, 0, 0);
    lv_obj_clear_flag(full_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *scr = full_root;

    int LX = 0;

    /* Знак */
    sign_obj = lv_obj_create(scr);
    /* Знак — максимальный круг для левой зоны: 64 px (до значка ACC/LKA на x=68),
       красное кольцо 7 px (~11 %, как у дорожного знака), белое поле 50 px */
    lv_obj_set_size(sign_obj, SIGN_D, SIGN_D);
    lv_obj_set_pos(sign_obj, LX + 1, 2);
    lv_obj_set_style_radius(sign_obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(sign_obj, lv_color_white(), 0);
    lv_obj_set_style_border_color(sign_obj, lv_color_hex(0xcc0000), 0);
    lv_obj_set_style_border_width(sign_obj, SIGN_RING, 0);
    lv_obj_clear_flag(sign_obj, LV_OBJ_FLAG_SCROLLABLE);
    sign_lbl = lv_label_create(sign_obj);
    lv_label_set_text(sign_lbl, "");
    lv_obj_set_style_text_color(sign_lbl, lv_color_black(), 0);
    lv_obj_center(sign_lbl);
    /* диагональ "конец ограничения" (координаты в круге 55x55, pad 0) */
    {
        static lv_point_t end_pts[2] = { {15, SIGN_D - 15}, {SIGN_D - 15, 15} };
        sign_end_line = lv_line_create(sign_obj);
        lv_line_set_points(sign_end_line, end_pts, 2);
        lv_obj_set_style_line_width(sign_end_line, 4, 0);
        lv_obj_set_style_line_color(sign_end_line, lv_color_hex(C_GRAY), 0);
        lv_obj_set_pos(sign_end_line, 0, 0);
        lv_obj_add_flag(sign_end_line, LV_OBJ_FLAG_HIDDEN);
    }
    /* «обгон запрещён»: левая машина красная, правая чёрная (знак 3.20) */
    sign_car_l = car_img(sign_obj, &img_sign_car, (SIGN_D - 38) / 2,      (SIGN_D - 15) / 2, C_SIGN_RED);
    sign_car_r = car_img(sign_obj, &img_sign_car, (SIGN_D - 38) / 2 + 20, (SIGN_D - 15) / 2, 0x000000);
    lv_obj_add_flag(sign_car_l, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(sign_car_r, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_pad_all(sign_obj, 0, 0);
    lv_obj_add_flag(sign_obj, LV_OBJ_FLAG_HIDDEN);
#if HUD_PSD_DIAG
    diag_build(scr);
#endif
#if HUD_CAMS
    cams_build(scr);
#endif

    /* ACC / LKA */
    /* Значок ACC / Lane Assist — слои-маски, цвет каждого задаётся отдельно */
    assist_box = lv_obj_create(scr);
    lv_obj_remove_style_all(assist_box);
    lv_obj_set_size(assist_box, 64, 54);
    lv_obj_set_pos(assist_box, LX + 68, 5);
    lv_obj_clear_flag(assist_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    /* иконка — те же слои, что в минималистичном режиме (tools/gen_min_images.py), уменьшенные до 64 x 50 */
    as_line_l = car_img(assist_box, &img_f_lane_l, F_LANE_L_X, F_LANE_L_Y + 2, C_GREEN);
    as_line_r = car_img(assist_box, &img_f_lane_r, F_LANE_R_X, F_LANE_R_Y + 2, C_GREEN);
    as_arcs   = car_img(assist_box, &img_f_arcs,   F_ARCS_X,   F_ARCS_Y + 2,   C_GREEN);
    as_front  = car_img(assist_box, &img_f_lead,   F_LEAD_X,   F_LEAD_Y + 2,   C_GREEN);
    as_own    = car_img(assist_box, &img_f_own,    F_OWN_X,    F_OWN_Y + 2,    C_GREEN);
    lv_obj_add_flag(assist_box, LV_OBJ_FLAG_HIDDEN);

    /* Стрелка (заглушка текстом) */
    /* Стрелка навигации: картинка (hud_nav_images, 4-бит маски) + дистанция
       до манёвра под ней. Область 192x140: стрелка 192x116, строка — ниже. */
    /* анимированная стрелка (если есть /arrows.bin): кадр 192x130 на месте статичного */
    anim_img = lv_img_create(scr);
    lv_obj_set_pos(anim_img, LX + 145, 5 + ANIM_DY);
    lv_obj_add_flag(anim_img, LV_OBJ_FLAG_HIDDEN);

    arrow_ph = lv_obj_create(scr);
    lv_obj_remove_style_all(arrow_ph);
    lv_obj_set_size(arrow_ph, 192, 167);
    lv_obj_set_pos(arrow_ph, LX + 145, 5);
    lv_obj_clear_flag(arrow_ph, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    arrow_img = lv_img_create(arrow_ph);                     /* кадр стрелки с перспективой (индексная картинка со своей палитрой) */
    lv_img_set_src(arrow_img, nav_big.turn[0].img);
    lv_obj_set_pos(arrow_img, nav_big.turn[0].x, nav_big.turn[0].y);
    lv_obj_add_flag(arrow_img, LV_OBJ_FLAG_HIDDEN);
    next_img = car_img(arrow_ph, nav_small.turn[0].img, 0, 0, C_NAV_NEXT);   /* следующий манёвр */
    lv_obj_add_flag(next_img, LV_OBJ_FLAG_HIDDEN);
    arrow_lbl = lv_label_create(arrow_ph);
    lv_label_set_text(arrow_lbl, "...");
    lv_obj_set_style_text_font(arrow_lbl, &hud_font_route, 0);
    lv_obj_set_style_text_color(arrow_lbl, lv_color_hex(C_GRAY), 0);
    lv_obj_align(arrow_lbl, LV_ALIGN_CENTER, 0, -12);
    lv_obj_add_flag(arrow_lbl, LV_OBJ_FLAG_HIDDEN);
    nav_dist_lbl = lv_label_create(arrow_ph);
    lv_label_set_text(nav_dist_lbl, "");
    lv_obj_set_style_text_font(nav_dist_lbl, &hud_font_route, 0);
    lv_obj_set_style_text_color(nav_dist_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_align(nav_dist_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(nav_dist_lbl, 192);
    lv_obj_set_pos(nav_dist_lbl, 0, NAV_BOTTOM_Y - 22);
    lv_obj_add_flag(nav_dist_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(arrow_ph, LV_OBJ_FLAG_HIDDEN);

    /* Машинка с открытыми элементами — в области стрелки */
    car_box = lv_obj_create(scr0);
    lv_obj_remove_style_all(car_box);
    lv_obj_set_size(car_box, 192, 140);
    lv_obj_set_pos(car_box, LX + 145, 5);
    lv_obj_clear_flag(car_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    car_img(car_box, &img_car_body,   CAR_BODY_X,   CAR_BODY_Y,   C_CAR_BODY);
    car_img(car_box, &img_car_lights, CAR_LIGHTS_X, CAR_LIGHTS_Y, C_CAR_LIGHTS);
    {
        static const lv_img_dsc_t *under[6] = { &img_car_fl_under, &img_car_fr_under, &img_car_rl_under,
                                                &img_car_rr_under, &img_car_hood_under, &img_car_trunk_under };
        static const lv_img_dsc_t *part[6]  = { &img_car_fl, &img_car_fr, &img_car_rl,
                                                &img_car_rr, &img_car_hood, &img_car_trunk };
        static const int16_t ux[6] = { CAR_FL_UNDER_X, CAR_FR_UNDER_X, CAR_RL_UNDER_X, CAR_RR_UNDER_X, CAR_HOOD_UNDER_X, CAR_TRUNK_UNDER_X };
        static const int16_t uy[6] = { CAR_FL_UNDER_Y, CAR_FR_UNDER_Y, CAR_RL_UNDER_Y, CAR_RR_UNDER_Y, CAR_HOOD_UNDER_Y, CAR_TRUNK_UNDER_Y };
        static const int16_t px[6] = { CAR_FL_X, CAR_FR_X, CAR_RL_X, CAR_RR_X, CAR_HOOD_X, CAR_TRUNK_X };
        static const int16_t py[6] = { CAR_FL_Y, CAR_FR_Y, CAR_RL_Y, CAR_RR_Y, CAR_HOOD_Y, CAR_TRUNK_Y };
        for (int i = 0; i < 6; i++) {
            car_under[i] = car_img(car_box, under[i], ux[i], uy[i], 0x000000);
            car_part[i]  = car_img(car_box, part[i],  px[i], py[i], C_CAR_OPEN);
            lv_obj_add_flag(car_under[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int i = 0; i < 6; i++) lv_obj_add_flag(car_part[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(car_box, LV_OBJ_FLAG_HIDDEN);

    /* Полоска расхода — правый край экрана, 5 px */
    fuel_bar = lv_obj_create(scr);
    lv_obj_remove_style_all(fuel_bar);
    lv_obj_set_size(fuel_bar, 5, 1);
    lv_obj_set_pos(fuel_bar, 640 - 5, 171);
    lv_obj_set_style_bg_color(fuel_bar, lv_color_hex(C_FUEL), 0);
    lv_obj_set_style_bg_opa(fuel_bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(fuel_bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(fuel_bar, LV_OBJ_FLAG_HIDDEN);

    /* Pre sense: красный треугольник + надпись, поверх стрелки и машинки */
    ps_box = lv_obj_create(scr0);
    lv_obj_remove_style_all(ps_box);
    lv_obj_set_size(ps_box, 192, 140);
    lv_obj_set_pos(ps_box, LX + 145, 5);
    lv_obj_clear_flag(ps_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    ps_tri = car_img(ps_box, &img_presense_tri, (192 - 92) / 2, 6, 0xe01414);
    car_img(ps_box, &img_presense_txt, (192 - img_presense_txt.header.w) / 2, 98, 0xffffff);
    lv_obj_add_flag(ps_box, LV_OBJ_FLAG_HIDDEN);

    /* Бар дистанции */
    {
#if HUD_NAV_BAR_TOP
        top_bar = lv_obj_create(scr);
        lv_obj_remove_style_all(top_bar);
        lv_obj_set_size(top_bar, TOP_BAR_W, TOP_BAR_H);
        lv_obj_set_pos(top_bar, LX + 145, 1);
        lv_obj_set_style_radius(top_bar, TOP_BAR_H / 2, 0);
        lv_obj_set_style_bg_color(top_bar, lv_color_hex(C_BAR_OFF), 0);
        lv_obj_set_style_bg_opa(top_bar, LV_OPA_COVER, 0);
        lv_obj_clear_flag(top_bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(top_bar, LV_OBJ_FLAG_HIDDEN);
        top_fill = lv_obj_create(top_bar);
        lv_obj_remove_style_all(top_fill);
        lv_obj_set_size(top_fill, 1, TOP_BAR_H);
        lv_obj_set_pos(top_fill, 0, 0);
        lv_obj_set_style_radius(top_fill, TOP_BAR_H / 2, 0);
        lv_obj_set_style_bg_color(top_fill, lv_color_hex(C_BAR_ON), 0);
        lv_obj_set_style_bg_opa(top_fill, LV_OPA_COVER, 0);
        lv_obj_clear_flag(top_fill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
#endif
        int bar_x = LX + 339, bar_y_bottom = 5 + 140;
        int seg_w = 14, seg_h = 7, seg_gap = 2;
        for (int i = 0; i < 16; i++) {
            lv_obj_t *seg = lv_obj_create(scr);
            lv_obj_set_size(seg, seg_w, seg_h);
            lv_obj_set_pos(seg, bar_x, bar_y_bottom - (i + 1) * (seg_h + seg_gap));
            lv_obj_set_style_border_width(seg, 0, 0);
            lv_obj_set_style_radius(seg, 1, 0);
            lv_obj_set_style_bg_color(seg, lv_color_hex(C_BAR_OFF), 0);
            lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(seg, LV_OBJ_FLAG_HIDDEN);
            bar_seg[i] = seg;
        }
    }

    /* Маршрутная инфа */
    route_icon = lv_label_create(scr);
    lv_label_set_text(route_icon, LV_SYMBOL_GPS);
    lv_obj_set_style_text_color(route_icon, lv_color_white(), 0);
    lv_obj_set_pos(route_icon, LX + 5, 132);
    lv_obj_add_flag(route_icon, LV_OBJ_FLAG_HIDDEN);

    route_lbl = lv_label_create(scr);
    lv_label_set_text(route_lbl, "");
    lv_obj_set_style_text_font(route_lbl, &hud_font_route, 0);
    lv_obj_set_style_text_color(route_lbl, lv_color_white(), 0);
    lv_obj_set_pos(route_lbl, LX + 25, 126);
    lv_obj_add_flag(route_lbl, LV_OBJ_FLAG_HIDDEN);

#if HUD_TSR_STATE
    tsr_build(scr);
#endif

    /* Статус связи — под стрелкой, виден только без связи */
    pair_lbl = lv_label_create(scr);
    lv_label_set_text(pair_lbl, "");
    lv_label_set_long_mode(pair_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(pair_lbl, 344);
    lv_obj_set_pos(pair_lbl, 8, 34);
    lv_obj_set_style_text_font(pair_lbl, &hud_font_menu, 0);
    lv_obj_set_style_text_color(pair_lbl, lv_color_hex(C_ORANGE), 0);
    lv_obj_add_flag(pair_lbl, LV_OBJ_FLAG_HIDDEN);

    /* Индикатор источника данных — под стрелкой навигации, по центру её области
       (внизу, под дистанцией). Выровнен по центру области 192 px. */
    link_lbl = lv_label_create(scr);
    lv_label_set_text(link_lbl, LV_SYMBOL_BLUETOOTH " ...");
    lv_obj_set_style_text_color(link_lbl, lv_color_hex(0x666666), 0);
    lv_obj_set_pos(link_lbl, 480, 4);                          /* x подбирается в апдейте: середина между заправкой и передачей */

    /* ===== Правая зона ===== */
    int RX = 360;

    /* Скорость: фиксированная ширина под 3 цифры, выравнивание вправо,
       правый край = 376 + 256 = 632. v9: на 20 px ниже (y=20). */
    /* обводка цифр скорости: 8 красных копий со сдвигом, под основной надписью */
    {
        static const int8_t off[8][2] = { {-3,0},{3,0},{0,-3},{0,3},{-2,-2},{2,-2},{-2,2},{2,2} };
        for (int i = 0; i < 8; i++) {
            lv_obj_t *o = lv_label_create(scr);
            lv_label_set_text(o, "");
            lv_obj_set_style_text_font(o, &hud_font_speed, 0);
            lv_obj_set_style_text_color(o, lv_color_hex(C_OVERSPEED), 0);
            lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_RIGHT, 0);
            lv_label_set_long_mode(o, LV_LABEL_LONG_CLIP);
            lv_obj_set_width(o, 256);
            lv_obj_set_pos(o, 376 + off[i][0], 20 + off[i][1]);
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
            spd_outline[i] = o;
        }
    }

    speed_lbl = lv_label_create(scr);
    lv_label_set_text(speed_lbl, "");
    lv_obj_set_style_text_font(speed_lbl, &hud_font_speed, 0);
    lv_obj_set_style_text_color(speed_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_align(speed_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(speed_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(speed_lbl, 256);
    lv_obj_set_pos(speed_lbl, 376, 20);
    lv_obj_add_flag(speed_lbl, LV_OBJ_FLAG_HIDDEN);

    /* Режим КП — над скоростью, серым, правый край по цифрам скорости
       (у цифр скорости ~6 px правого поля внутри глифа) */
    gear_lbl = lv_label_create(scr);
    lv_label_set_text(gear_lbl, "");
    lv_obj_set_style_text_font(gear_lbl, &hud_font_gear, 0);
    lv_obj_set_style_text_color(gear_lbl, lv_color_hex(C_LABEL_GRAY), 0);
    lv_obj_set_style_text_align(gear_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(gear_lbl, 250);
    lv_obj_set_pos(gear_lbl, 376, 1);
    lv_obj_add_flag(gear_lbl, LV_OBJ_FLAG_HIDDEN);

    /* Сколько заправить до полного — над зоной скорости слева, на одной линии
       с режимом КП: маленькая колонка АЗС + «13 л» / «3.4 gal», серым */
    tank_icon = car_img(scr, &img_fuel_icon, 376, 2, C_LABEL_GRAY);
    lv_obj_add_flag(tank_icon, LV_OBJ_FLAG_HIDDEN);
    tank_lbl = lv_label_create(scr);
    lv_label_set_text(tank_lbl, "");
    lv_obj_set_style_text_font(tank_lbl, &hud_font_fuel, 0);
    lv_obj_set_style_text_color(tank_lbl, lv_color_hex(C_LABEL_GRAY), 0);
    lv_obj_set_pos(tank_lbl, 376 + 15 + 5, 1);                 /* базовая линия — как у режима КП */
    lv_obj_add_flag(tank_lbl, LV_OBJ_FLAG_HIDDEN);

    acc_dot = car_img(scr, &img_acc_set, RX + 4, 145, C_GREEN);
    lv_obj_add_flag(acc_dot, LV_OBJ_FLAG_HIDDEN);

    acc_speed_lbl = lv_label_create(scr);
    lv_label_set_text(acc_speed_lbl, "");
    lv_obj_set_style_text_font(acc_speed_lbl, &hud_font_small, 0);
    lv_obj_set_style_text_color(acc_speed_lbl, lv_color_hex(0xaaaaaa), 0);
    lv_obj_set_pos(acc_speed_lbl, RX + 38, 146);
    lv_obj_add_flag(acc_speed_lbl, LV_OBJ_FLAG_HIDDEN);

    /* Лимитер — надпись LIM на месте значка ACC, по центру высоты цифр скорости */
    lim_icon = car_img(scr, &img_limiter, RX + 4, 149, 0xffffff);
    lv_obj_add_flag(lim_icon, LV_OBJ_FLAG_HIDDEN);

    /* Режим пробки — справа от скорости ACC */
    jam_icon = car_img(scr, &img_traffic_jam, RX + 102, 148, C_GREEN);
    lv_obj_add_flag(jam_icon, LV_OBJ_FLAG_HIDDEN);

    /* подпись единиц: выравнивание вправо, правый край 621 (как у прежней "km/h") */
    kmh_lbl = lv_label_create(scr);
    lv_label_set_text(kmh_lbl, "");
    lv_obj_set_width(kmh_lbl, 125);
    lv_obj_set_style_text_align(kmh_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(kmh_lbl, &hud_font_kmh, 0);
    lv_obj_set_style_text_color(kmh_lbl, lv_color_hex(C_LABEL_GRAY), 0);
    lv_obj_set_pos(kmh_lbl, 621 - 125, 133);

    mn_build(scr0);                        /* минималистичный режим */

    /* Мигалки */
    turn_left_container  = make_blinker(scr0, BLINK_L_X, BLINK_Y,
                                        &img_blink_l_outline, &img_blink_l_fill, &turn_left_sym);
    turn_right_container = make_blinker(scr0, BLINK_R_X, BLINK_Y,
                                        &img_blink_r_outline, &img_blink_r_fill, &turn_right_sym);

    splash_build(scr0);                    /* заставка без связи: поверх экрана, под меню и экраном обновления */
#if HUD_TOUCH_ENABLE
    build_menu(scr0);                      /* ловушка касаний и меню поверх всего */
#endif
    if (hud_ota_mode()) build_ota(scr0);    /* режим обновления: свой экран поверх всего */
    mode_apply();
    lv_timer_create(hud_update_cb, 50, NULL);
    lv_timer_create(anim_timer_cb, 16, NULL);
#if HUD_LOG_MEM
    lv_timer_create(mem_timer_cb, 10000, NULL);
#endif
    if (HUD_LOG_HUD) arduino_printf("[hud] экран построен, lv_timer 50 мс запущен\n");
}
