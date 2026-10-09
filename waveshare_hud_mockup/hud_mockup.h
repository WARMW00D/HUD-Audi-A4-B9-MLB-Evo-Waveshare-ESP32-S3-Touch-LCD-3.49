#ifndef HUD_MOCKUP_H
#define HUD_MOCKUP_H
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Строит экран и запускает lv_timer обновления (50 мс).
   Вызывается из lvgl_port_init() под мьютексом LVGL. */
void build_hud_mockup(void);

/* Язык и единицы во время работы (HUD_LANG_* / HUD_UNITS_* из hud_config.h).
   Можно вызывать из любой задачи: применяются на следующем тике экрана. */
void    hud_set_lang(uint8_t lang);
void    hud_set_units(uint8_t units);
uint8_t hud_get_lang(void);
uint8_t hud_get_units(void);
void    hud_set_psd(bool on);          /* знаки из PSD (прогноз маршрута MIB) вкл/выкл  */
bool    hud_get_psd(void);
void    hud_set_vze(bool on);          /* знаки из VZE_01 (камера / карта приборки)     */
bool    hud_get_vze(void);
void    hud_set_gallons(bool on);      /* «сколько заправить» в галлонах (true) или литрах */
bool    hud_get_gallons(void);
void    hud_set_fuel_avg(bool on);     /* полоска расхода: true — средний, false — мгновенный */
bool    hud_get_fuel_avg(void);
void    hud_set_accel_bar(bool on);    /* бар ускорения вкл/выкл                        */
bool    hud_get_accel_bar(void);

void    hud_set_overspeed_tol(uint8_t kmh);   /* допуск превышения, км/ч, 0..20 */
uint8_t hud_get_overspeed_tol(void);

/* hud_settings.cpp: загрузка/сохранение настроек во флеше (NVS) */
void hud_settings_load(void);
void hud_settings_save(void);

#ifdef __cplusplus
}
#endif
#endif
