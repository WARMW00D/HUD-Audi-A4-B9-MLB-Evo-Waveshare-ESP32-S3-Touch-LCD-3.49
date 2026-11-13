/*
  sim_core.h — ядро симулятора HUD для ПК
  ------------------------------------------------------------------------------
  Код экрана (hud_mockup.c), декодер (can_decode.c), PSD, камеры и настройки (hud_settings.cpp)
  собираются из папки прошивки БЕЗ изменений. Здесь — всё, что в прошивке привязано
  к железу: время, экран, касание, звук, настройки, а также проигрыватель логов
  сниффера (can_log_NNNN.txt). Ядро не зависит от ОС; окно и файлы — в sim_win.c.
*/
#ifndef SIM_CORE_H
#define SIM_CORE_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#define SIM_W 640
#define SIM_H 172

/* ---- экран ---- */
extern uint32_t sim_fb[SIM_W * SIM_H];   /* 0x00RRGGBB, сверху вниз */
extern volatile bool sim_fb_dirty;       /* ядро ставит при перерисовке, окно сбрасывает */
extern int sim_backlight;                /* яркость подсветки 0..255, которую выставил HUD; -1 — ещё не выставлял */

uint32_t sim_lv_tick(void);              /* для LV_TICK_CUSTOM */

void sim_init(void);                     /* настройки, LVGL, экран HUD */
void sim_step(uint32_t real_dt_ms);      /* вызывать часто (5-15 мс): время, кадры лога, LVGL */

/* ---- платформа автомобиля (карта CAN-сигналов декодера, HUD_PLATFORM в прошивке) ---- */
#define SIM_PLATFORM_MLB_EVO 1
#define SIM_PLATFORM_MQB     2
void sim_set_platform(int p);            /* можно на ходу: текущая точка лога проигрывается заново новым декодером */
int  sim_platform(void);

/* ---- режим экрана (HUD_MODE_FULL / HUD_MODE_MIN, то же, что кнопка «Режим» в меню HUD) ---- */
void sim_set_mode(int m);                /* переключается мгновенно, выбор сохраняется в настройках */
int  sim_mode(void);

/* ---- касание (мышь) ---- */
void sim_touch(int x, int y, bool down); /* координаты экрана HUD 0..639 / 0..171 */
void sim_request_menu(void);             /* как двойное касание */

/* ---- проигрыватель ---- */
/* Разобрать текст лога (буфер можно освободить сразу после вызова). cont = true — это следующий файл
   той же поездки: если метки времени продолжаются, состояние HUD не сбрасывается. */
bool   sim_load_log(const char *text, size_t len, bool cont);
bool   sim_loaded(void);
void   sim_play(bool on);
bool   sim_playing(void);
void   sim_set_speed(double k);
double sim_speed(void);
uint32_t sim_pos_ms(void);               /* позиция от начала файла */
uint32_t sim_dur_ms(void);
void   sim_seek_ms(uint32_t pos_ms);     /* с «разгоном»: 30 с до точки проигрываются мгновенно */
bool   sim_take_eof(void);               /* файл доигран (флаг сбрасывается при чтении) */
uint32_t sim_frames_total(void);
uint32_t sim_frames_pos(void);

/* метки навигации в файле (из BAP Navigation_SD), время — мс от начала файла */
typedef struct { uint32_t t; uint8_t main, dir; } sim_man_t;
const sim_man_t *sim_maneuvers(uint32_t *count);              /* смены манёвра (fct 0x17) */
typedef struct { uint32_t t0, t1; } sim_span_t;
const sim_span_t *sim_nav_spans(uint32_t *count);             /* ведение по маршруту активно (fct 0x11 = 1) */

/* ---- данные из файлов (буфер должен жить всегда) ---- */
bool sim_set_cams(const void *buf, size_t len);               /* cams.bin */

/* ---- то, что даёт платформа (sim_win.c / sim_headless.c) ---- */
void sim_plat_log(const char *utf8);                          /* строка журнала HUD */
void sim_plat_play(const int16_t *pcm, uint32_t samples, uint32_t rate, uint8_t volume);
int  sim_plat_cfg_get(const char *key, int def);              /* настройки HUD (вместо NVS) */
void sim_plat_cfg_set(const char *key, int val);

#ifdef __cplusplus
}
#endif
#endif
