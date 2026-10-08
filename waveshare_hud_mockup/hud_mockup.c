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
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "hud_config.h"
#include "hud_mockup.h"
#include "hud_data.h"
#include "vze_table.h"
#include "hud_images.h"
#include "hud_nav_images.h"
#include "hud_sound.h"
#include "hud_light.h"
#include "hud_ota.h"
#include "hud_source.h"
#include "hud_sounds.h"
#include "psd_speedlimit.h"
#include "src/lcd_bl_bsp/lcd_bl_pwm_bsp.h"

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
#define C_ACC_GO     0x2ee62e   /* бар: разгон      */
#define C_ACC_BRAKE  0xe01414   /* бар: торможение  */
#define C_FUEL       0x505050   /* полоска расхода  */
#define C_OVERSPEED  0xff1a1a   /* обводка скорости при превышении */
#define C_SWA        0xff2020   /* Side Assist                     */
#define C_SIGN_RED   0xcc0000
#define SIGN_D       64    /* диаметр знака         */
#define SIGN_RING    7     /* толщина красного кольца */
#define C_SIGN_WARN  0xff8080
#define C_NAV_ARROW  0xeaf4ff
#define C_NAV_NEXT   0xa8a8a8
#define LINK_BOTTOM_MARGIN 2     /* отступ значка источника от нижнего края экрана, px */
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

/* бар ускорения: ACC_SEG квадратов в каждую сторону от центра области стрелки */
#define ACC_SEG      8
#define ACC_SQ       8
#define ACC_PITCH    11
#define ACC_GAP_MID  3
#define ACC_Y        (172 - ACC_SQ)   /* самый низ экрана, подальше от дистанции до манёвра */
static lv_obj_t *acc_sq[2][ACC_SEG];
static lv_obj_t *fuel_bar;               /* расход топлива, правый край */
static lv_obj_t *arrow_ph, *arrow_img, *arrow_lbl, *nav_dist_lbl, *next_img;
static lv_obj_t *bar_seg[16];
static lv_obj_t *route_icon, *route_lbl;
static lv_obj_t *speed_lbl;
static lv_obj_t *acc_dot, *acc_speed_lbl;   /* acc_dot — значок установки скорости ACC */
static lv_obj_t *turn_left_container, *turn_right_container;
static lv_obj_t *turn_left_sym, *turn_right_sym;
static lv_obj_t *gear_lbl, *kmh_lbl, *tank_icon, *tank_lbl;
static lv_obj_t *car_box, *car_part[6], *car_under[6];   /* FL FR RL RR HOOD TRUNK — как биты DOOR_* */
static lv_obj_t *link_lbl, *pair_lbl;

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

typedef struct { uint8_t mode; uint16_t val; } SignItem;

/* Код VZE -> знак. allow_fallback: для основного знака можно взять ACC_Tempolimit
   и показать сырой код; для дополнительных знаков 2/3 — только то, что точно известно. */
static SignItem vze_eval(uint8_t code, bool suppress, uint8_t tl, bool allow_fallback)
{
    SignItem it = { SIGN_HIDE, 0 };
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
static volatile bool    g_accel = HUD_ACCEL_BAR;
static volatile bool    g_vze   = HUD_VZE_SIGNS;
static volatile bool    g_favg  = HUD_FUEL_AVG;
static volatile bool    g_gal   = HUD_VOLUME_GAL;
void    hud_set_gallons(bool on)     { g_gal = on; }
bool    hud_get_gallons(void)        { return g_gal; }
void    hud_set_vze(bool on)         { g_vze = on; }
bool    hud_get_vze(void)            { return g_vze; }
void    hud_set_fuel_avg(bool on)    { g_favg = on; }
bool    hud_get_fuel_avg(void)       { return g_favg; }
void    hud_set_psd(bool on)         { g_psd = on; }
bool    hud_get_psd(void)            { return g_psd; }
void    hud_set_accel_bar(bool on)   { g_accel = on; }
bool    hud_get_accel_bar(void)      { return g_accel; }
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
static void hud_update_cb(lv_timer_t *t)
{
    (void)t;
    HudData d;
    hud_data_snapshot(&d);
    uint32_t now = hud_now_ms();
    char buf[32];
    (void)now;
    uint16_t lim_kmh_for_speed = 0;

    /* --- подпись единиц скорости (язык / единицы могут смениться на ходу) --- */
    set_text(kmh_lbl, g_units == HUD_UNITS_MI ? "mph" : txt()->kmh);

    /* --- бар ускорения по CAN: зелёный — разгон, красный — торможение ---
       ESP_02 (если есть на шине), иначе производная скорости Kombi_01 */
    {
        static const float amax = (HUD_ACCEL_REF_KMH / 3.6f) / HUD_ACCEL_REF_SEC;
        static const float bmax = (float)HUD_BRAKE_FULL_MS2;
        int lit = 0;
        uint32_t col = C_ACC_GO;
        bool have = true;
        float a = 0;
        if (d.valid & V_ACCEL)       a = d.accel_esp_x100 / 100.0f;
        else if (d.valid & V_SPDACC) a = d.accel_spd_x100 / 100.0f;
        else                         have = false;

        /* сглаживание (EMA, постоянная времени HUD_ACCEL_SMOOTH_MS) */
        static float af = 0;
        static int8_t dir = 0;                    /* 0 — пусто, +1 — разгон, -1 — торможение */
        const float k = 50.0f / (50.0f + HUD_ACCEL_SMOOTH_MS);
        bool moving = g_accel && have && (d.valid & V_SPEED) && d.speed_kmh > 0;
        af = moving ? af + (a - af) * k : 0.0f;

        /* гистерезис: включается выше HUD_ACCEL_ON, гаснет ниже HUD_ACCEL_OFF;
           смена разгон <-> торможение только через "пусто" */
        if (!moving)                                   dir = 0;
        else if (dir == 0 && af >  HUD_ACCEL_ON)       dir = +1;
        else if (dir == 0 && af < -HUD_ACCEL_ON)       dir = -1;
        else if (dir == +1 && af <  HUD_ACCEL_OFF)     dir = 0;
        else if (dir == -1 && af > -HUD_ACCEL_OFF)     dir = 0;

        if (dir != 0) {
            float ag = (dir > 0 ? af : -af) * HUD_ACCEL_GAIN;
            lit = (int)(ag / (dir > 0 ? amax : bmax) * ACC_SEG + 0.5f);
            if (lit < 1) lit = 1;
            if (lit > ACC_SEG) lit = ACC_SEG;
            col = dir > 0 ? C_ACC_GO : C_ACC_BRAKE;
        }
        for (int i = 0; i < ACC_SEG; i++)
            for (int sd = 0; sd < 2; sd++) {
                if (i < lit) set_bg_color(acc_sq[sd][i], col);
                vis(acc_sq[sd][i], i < lit);
            }
    }

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
        if (g_favg) {
            if ((d.valid & V_BC) && d.bc_avg_x10 != 0xFFFF)          { fv = d.bc_avg_x10;   show = true; }
            else if ((d.valid & V_FUEL) && d.fuel_avg_x10 != 0xFFFF) { fv = d.fuel_avg_x10; show = true; }
        } else {
            fv = d.fuel_l100_x10; show = (d.valid & V_FUEL) && fast;
        }
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
    /* индикатор источника под стрелкой: нет связи — серый «BT ...», BLE — тёмно-синий,
       свой CAN — тёмно-зелёная надпись CAN */
    {
        int src = d.link_up ? hud_src_current() : -1;
        if (src == 1)      { set_text(link_lbl, "CAN");               set_text_color(link_lbl, C_LINK_CAN); }
        else if (src == 0) { set_text(link_lbl, LV_SYMBOL_BLUETOOTH); set_text_color(link_lbl, C_LINK_BLE); }
        else               { set_text(link_lbl, LV_SYMBOL_BLUETOOTH " ..."); set_text_color(link_lbl, 0x666666); }
        vis(link_lbl, !pf);
    }
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
    bool arcs = obj && (d.acc_status == 3 || d.acc_status == 4);
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

        SignItem main = { SIGN_HIDE, 0 };
        if (psd_expl)                                        { main.mode = SIGN_LIMIT; main.val = pe_kmh; }
        if (main.mode == SIGN_HIDE && sv)                    main = vze_eval(d.sign_raw, d.sign_suppress, tl, true);
        if (main.mode == SIGN_HIDE && (P.src == PSD_LIM_LEGAL || P.src == PSD_LIM_ENDED))
                                                             { main.mode = SIGN_LIMIT; main.val = P.kmh; }
        if (main.mode != SIGN_HIDE) items[n_items++] = main;

        if (sv) {
            SignItem x2 = vze_eval(d.sign_raw2, d.sign_sup2, 0, false);
            SignItem x3 = vze_eval(d.sign_raw3, d.sign_sup3, 0, false);
            if (x2.mode == SIGN_LIMIT && x2.val != main.val) items[n_items++] = x2;
            if (x3.mode == SIGN_LIMIT && x3.val != main.val && x3.val != x2.val) items[n_items++] = x3;
        }
        if ((psd_expl && pe_novt) || (sv && (vze_eval(d.sign_raw2, d.sign_sup2, 0, false).mode == SIGN_NOOVT ||
                                             vze_eval(d.sign_raw3, d.sign_sup3, 0, false).mode == SIGN_NOOVT)))
            if (n_items < 4) items[n_items++] = (SignItem){ SIGN_NOOVT, 0 };

        /* ограничение для контроля превышения — всегда основной знак */
        lim_kmh_for_speed = (main.mode == SIGN_LIMIT) ? main.val : 0;
    }

    int smode = SIGN_HIDE; uint16_t sval = 0;
    if (n_items) {
        SignItem it = items[(now / HUD_SIGN_CYCLE_MS) % n_items];
        smode = it.mode; sval = it.val;
    }
    if (smode != SIGN_HIDE) {
        bool warn_ph = d.sign_warn && (now % 500) < 250;
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
            float l = HUD_TANK_L * (100u - d.bc_fuel_pct) / 100.0f;
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
            lv_obj_set_pos(next_img, NAV_AREA_W - NAV_SMALL_W - 2 + n2->x, 140 - NAV_SMALL_H - 1 + n2->y);
        }
        n2_last = n2;
        vis(next_img, n2 != NULL);
    }
    vis(arrow_ph, nav);
    if (nav) {
        static const NavImg *ni_last = NULL;
        if (ni && ni != ni_last) {
            lv_img_set_src(arrow_img, ni->img);
            lv_obj_set_pos(arrow_img, ni->x, ni->y);
        }
        ni_last = ni;
        bool calc = (d.man_main == 0x09 || d.man_main == 0x0A);
        vis(arrow_img, ni != NULL);
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
    if (nav && (d.valid & V_MDIST) && d.bargraph != 0xFF)
        filled = d.bargraph * 16 / 100;
    for (int i = 0; i < 16; i++) {
        vis(bar_seg[i], filled >= 0);
        if (filled >= 0) set_bg_color(bar_seg[i], i < filled ? C_BAR_ON : C_BAR_OFF);
    }

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
   нажатие на серый переключает. Переключатели (VZE, PSD, Разгон): зелёный — вкл,
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

enum { B_LANG_RU, B_LANG_EN, B_UNIT_KM, B_UNIT_MI, B_VOL_L, B_VOL_G, B_FUEL_I, B_FUEL_A,
       B_VZE, B_PSD, B_ACCEL, B_SND, B_TOL, B_LDR, B_COUNT };
enum { G_LANG, G_UNITS, G_VOL, G_FUEL, G_OPT, G_MORE, G_COUNT };
static lv_obj_t *menu_box, *menu_title[G_COUNT], *menu_btn[B_COUNT], *menu_btn_lbl[B_COUNT], *menu_close_lbl;
static uint32_t  menu_last_touch;

/* панель ползунка: 0 — громкость, 1 — допуск превышения */
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

static void menu_refresh(void)
{
    bool en = g_lang == HUD_LANG_EN, mi = g_units == HUD_UNITS_MI;
    static const char *t_ru[G_COUNT] = { "Язык", "Единицы", "Объём", "Расход", "Опции", "Прочее" };
    static const char *t_en[G_COUNT] = { "Language", "Units", "Volume", "Fuel", "Options", "More" };
    for (int g = 0; g < G_COUNT; g++) lv_label_set_text(menu_title[g], en ? t_en[g] : t_ru[g]);

    lv_label_set_text(menu_btn_lbl[B_LANG_RU], "Русский");
    lv_label_set_text(menu_btn_lbl[B_LANG_EN], "English");
    lv_label_set_text(menu_btn_lbl[B_UNIT_KM], en ? "km" : "км");
    lv_label_set_text(menu_btn_lbl[B_UNIT_MI], en ? "miles" : "мили");
    lv_label_set_text(menu_btn_lbl[B_VOL_L],   en ? "liters" : "литры");
    lv_label_set_text(menu_btn_lbl[B_VOL_G],   en ? "gallons" : "галлоны");
    lv_label_set_text(menu_btn_lbl[B_FUEL_I],  en ? "Instant" : "Мгнов.");
    lv_label_set_text(menu_btn_lbl[B_FUEL_A],  en ? "Average" : "Средний");
    lv_label_set_text(menu_btn_lbl[B_VZE], "VZE");
    lv_label_set_text(menu_btn_lbl[B_PSD], "PSD");
    lv_label_set_text(menu_btn_lbl[B_ACCEL], en ? "Accel" : "Разгон");
    char buf[32];
    snprintf(buf, sizeof buf, "%s %u", en ? "Sound" : "Звук", (unsigned)hud_sound_get_volume());
    lv_label_set_text(menu_btn_lbl[B_SND], buf);
    snprintf(buf, sizeof buf, "%s %u", en ? "Margin" : "Допуск", (unsigned)g_tol);
    lv_label_set_text(menu_btn_lbl[B_TOL], buf);

    menu_sel(B_LANG_RU, !en);        menu_sel(B_LANG_EN, en);
    menu_sel(B_UNIT_KM, !mi);        menu_sel(B_UNIT_MI, mi);
    menu_sel(B_VOL_L,   !g_gal);     menu_sel(B_VOL_G,   g_gal);
    menu_sel(B_FUEL_I,  !g_favg);    menu_sel(B_FUEL_A,  g_favg);
    menu_sw(B_VZE, g_vze);           menu_sw(B_PSD, g_psd);          menu_sw(B_ACCEL, g_accel);
    menu_sel(B_SND, false);          menu_sel(B_TOL, false);
    lv_label_set_text(menu_btn_lbl[B_LDR], en ? "Light sens." : "Датчик");
    menu_sel(B_LDR, hud_light_enabled());
    lv_label_set_text(menu_close_lbl, LV_SYMBOL_CLOSE);
}

/* ---- панель ползунка ---- */
static void ov_labels(void)
{
    bool en = g_lang == HUD_LANG_EN;
    int  v  = ov_kind == 0 ? (int)hud_sound_get_volume() : (int)g_tol;
    char buf[48];
    lv_label_set_text(ov_title, ov_kind == 0 ? (en ? "Sound volume" : "Громкость звука")
                                             : (en ? "Overspeed margin, km/h" : "Допуск превышения, км/ч"));
    snprintf(buf, sizeof buf, "%d", v);
    lv_label_set_text(ov_val, buf);
    lv_label_set_text(ov_ok_lbl, en ? "OK" : "Готово");
#if LV_USE_SLIDER
    lv_slider_set_value(ov_slider, v, LV_ANIM_OFF);
#endif
}

static void ov_set(int v)
{
    int mx = ov_kind == 0 ? 100 : 20;
    if (v < 0) v = 0;
    if (v > mx) v = mx;
    if (ov_kind == 0) hud_sound_set_volume((uint8_t)v);
    else              hud_set_overspeed_tol((uint8_t)v);
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
    lv_slider_set_range(ov_slider, 0, kind == 0 ? 100 : 20);
    lv_slider_set_value(ov_slider, kind == 0 ? (int)hud_sound_get_volume() : (int)g_tol, LV_ANIM_OFF);
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
    ov_set((ov_kind == 0 ? (int)hud_sound_get_volume() : (int)g_tol) + dir * step);
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
    case B_FUEL_I:  hud_set_fuel_avg(false); break;
    case B_FUEL_A:  hud_set_fuel_avg(true); break;
    case B_VZE:     hud_set_vze(!g_vze); break;
    case B_PSD:     hud_set_psd(!g_psd); break;
    case B_ACCEL:   hud_set_accel_bar(!g_accel); break;
    case B_SND:     menu_last_touch = hud_now_ms(); ov_open(0); return;
    case B_TOL:     menu_last_touch = hud_now_ms(); ov_open(1); return;
    case B_LDR:     menu_last_touch = hud_now_ms(); cal_open(); return;
    }
    menu_last_touch = hud_now_ms();
    menu_refresh();
    hud_settings_save();
}

static void menu_close(void) { ov_close(); cal_close(); lv_obj_add_flag(menu_box, LV_OBJ_FLAG_HIDDEN); }
static void menu_close_cb(lv_event_t *e) { (void)e; menu_close(); }

/* запрос меню от задачи тача (одиночное / двойное касание, HUD_MENU_DOUBLE_TAP) */
extern bool hud_touch_take_menu_request(void);

static void menu_open_cb(lv_event_t *e)
{
    (void)e;
    menu_refresh();
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

    /* 6 колонок по 98 px + кнопка закрытия 28 px */
    const int colw = 98, gap = 3, x0 = 3, ytop = 36, hfull = 128, hhalf = 61, h3 = 38;
    for (int g = 0; g < G_COUNT; g++) {
        int x = x0 + g * (colw + gap);
        menu_title[g] = lv_label_create(menu_box);
        lv_obj_set_style_text_font(menu_title[g], &hud_font_menu, 0);
        lv_obj_set_style_text_color(menu_title[g], lv_color_hex(C_MENU_DIM), 0);
        lv_obj_set_width(menu_title[g], colw);
        lv_obj_set_style_text_align(menu_title[g], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(menu_title[g], x, 7);
    }
    #define COLX(g) (x0 + (g) * (colw + gap))
    #define Y2(i)   (ytop + (i) * (hhalf + 6))
    #define Y3(i)   (ytop + (i) * (h3 + 6))
    #define MKB(id, g, y, h) menu_btn[id] = menu_mk_btn(menu_box, COLX(g), y, colw, h, &menu_btn_lbl[id])
    MKB(B_LANG_RU, G_LANG,  Y2(0), hhalf);  MKB(B_LANG_EN, G_LANG,  Y2(1), hhalf);
    MKB(B_UNIT_KM, G_UNITS, Y2(0), hhalf);  MKB(B_UNIT_MI, G_UNITS, Y2(1), hhalf);
    MKB(B_VOL_L,   G_VOL,   Y2(0), hhalf);  MKB(B_VOL_G,   G_VOL,   Y2(1), hhalf);
    MKB(B_FUEL_I,  G_FUEL,  Y2(0), hhalf);  MKB(B_FUEL_A,  G_FUEL,  Y2(1), hhalf);
    MKB(B_VZE,     G_OPT,   Y3(0), h3);     MKB(B_PSD,     G_OPT,   Y3(1), h3);
    MKB(B_ACCEL,   G_OPT,   Y3(2), h3);
    MKB(B_SND,     G_MORE,  Y3(0), h3);     MKB(B_TOL,     G_MORE,  Y3(1), h3);
    MKB(B_LDR,     G_MORE,  Y3(2), h3);
    #undef MKB
    #undef Y3
    #undef Y2
    for (int b = 0; b < B_COUNT; b++)
        lv_obj_add_event_cb(menu_btn[b], menu_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)b);
    (void)hfull;

    int xc = COLX(G_COUNT);
    lv_obj_t *cb = menu_mk_btn(menu_box, xc, 7, 640 - xc - 3, 158, &menu_close_lbl);
    lv_obj_set_style_text_font(menu_close_lbl, LV_FONT_DEFAULT, 0);           /* символ ✕ — во встроенном шрифте */
    lv_obj_add_event_cb(cb, menu_close_cb, LV_EVENT_CLICKED, NULL);
    #undef COLX

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

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

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

    /* ACC / LKA */
    /* Значок ACC / Lane Assist — слои-маски, цвет каждого задаётся отдельно */
    assist_box = lv_obj_create(scr);
    lv_obj_remove_style_all(assist_box);
    lv_obj_set_size(assist_box, 64, 54);
    lv_obj_set_pos(assist_box, LX + 68, 5);
    lv_obj_clear_flag(assist_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    as_line_l = car_img(assist_box, &img_assist_line_l, ASSIST_LINE_L_X, ASSIST_LINE_L_Y, C_GREEN);
    as_line_r = car_img(assist_box, &img_assist_line_r, ASSIST_LINE_R_X, ASSIST_LINE_R_Y, C_GREEN);
    as_arcs   = car_img(assist_box, &img_assist_arcs,   ASSIST_ARCS_X,   ASSIST_ARCS_Y,   C_GREEN);
    as_front  = car_img(assist_box, &img_assist_front,  ASSIST_FRONT_X,  ASSIST_FRONT_Y,  C_GREEN);
    as_own    = car_img(assist_box, &img_assist_own,    ASSIST_OWN_X,    ASSIST_OWN_Y,    C_GREEN);
    lv_obj_add_flag(assist_box, LV_OBJ_FLAG_HIDDEN);

    /* Стрелка (заглушка текстом) */
    /* Стрелка навигации: картинка (hud_nav_images, 4-бит маски) + дистанция
       до манёвра под ней. Область 192x140: стрелка 192x116, строка — ниже. */
    arrow_ph = lv_obj_create(scr);
    lv_obj_remove_style_all(arrow_ph);
    lv_obj_set_size(arrow_ph, 192, 140);
    lv_obj_set_pos(arrow_ph, LX + 145, 5);
    lv_obj_clear_flag(arrow_ph, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    arrow_img = car_img(arrow_ph, nav_big.turn[0].img, nav_big.turn[0].x, nav_big.turn[0].y, C_NAV_ARROW);
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
    lv_obj_set_pos(nav_dist_lbl, 0, NAV_AREA_H + 2);
    lv_obj_add_flag(nav_dist_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(arrow_ph, LV_OBJ_FLAG_HIDDEN);

    /* Машинка с открытыми элементами — в области стрелки */
    car_box = lv_obj_create(scr);
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

    /* Бар ускорения — по нижнему краю под стрелкой навигации, из центра в обе стороны */
    {
        int cx = LX + 145 + 192 / 2;
        for (int i = 0; i < ACC_SEG; i++)
            for (int sd = 0; sd < 2; sd++) {
                lv_obj_t *q = lv_obj_create(scr);
                lv_obj_remove_style_all(q);
                lv_obj_set_size(q, ACC_SQ, ACC_SQ);
                int x = sd ? cx + ACC_GAP_MID / 2 + 1 + i * ACC_PITCH
                           : cx - ACC_GAP_MID / 2 - 1 - ACC_SQ - i * ACC_PITCH;
                lv_obj_set_pos(q, x, ACC_Y);
                lv_obj_set_style_bg_color(q, lv_color_hex(0xe01414), 0);
                lv_obj_set_style_bg_opa(q, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(q, 1, 0);
                lv_obj_clear_flag(q, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_flag(q, LV_OBJ_FLAG_HIDDEN);
                acc_sq[sd][i] = q;
            }
    }

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
    ps_box = lv_obj_create(scr);
    lv_obj_remove_style_all(ps_box);
    lv_obj_set_size(ps_box, 192, 140);
    lv_obj_set_pos(ps_box, LX + 145, 5);
    lv_obj_clear_flag(ps_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    ps_tri = car_img(ps_box, &img_presense_tri, (192 - 92) / 2, 6, 0xe01414);
    car_img(ps_box, &img_presense_txt, (192 - img_presense_txt.header.w) / 2, 98, 0xffffff);
    lv_obj_add_flag(ps_box, LV_OBJ_FLAG_HIDDEN);

    /* Бар дистанции */
    {
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
    lv_obj_set_width(link_lbl, 192);
    lv_obj_set_style_text_align(link_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(link_lbl, lv_color_hex(0x666666), 0);
    lv_obj_align(link_lbl, LV_ALIGN_BOTTOM_LEFT, LX + 145, -LINK_BOTTOM_MARGIN);   /* от нижнего края экрана */

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

    /* Мигалки */
    turn_left_container  = make_blinker(scr, BLINK_L_X, BLINK_Y,
                                        &img_blink_l_outline, &img_blink_l_fill, &turn_left_sym);
    turn_right_container = make_blinker(scr, BLINK_R_X, BLINK_Y,
                                        &img_blink_r_outline, &img_blink_r_fill, &turn_right_sym);

#if HUD_TOUCH_ENABLE
    build_menu(scr);                       /* ловушка касаний и меню поверх всего */
#endif
    if (hud_ota_mode()) build_ota(scr);    /* режим обновления: свой экран поверх всего */
    lv_timer_create(hud_update_cb, 50, NULL);
    if (HUD_LOG_HUD) arduino_printf("[hud] экран построен, lv_timer 50 мс запущен\n");
}
