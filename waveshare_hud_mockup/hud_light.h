/*
  hud_light.h — фоторезистор на GPIO5 (запасной датчик освещённости)
  ------------------------------------------------------------------------------
  Схема: фоторезистор между 3V3 и GPIO5, резистор 10 кОм между GPIO5 и GND,
  конденсатор 100 нФ GPIO5 — GND. Работает только когда включён (меню →
  «Прочее» → «Датчик») и нет данных датчика RLS_01 по CAN.
*/
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void hud_light_poll(void);                   /* вызывать каждые ~50 мс (из update_brightness) */
void hud_light_force(bool on);               /* измерять даже когда выключен (панель калибровки) */
int  hud_light_mv(void);                     /* сглаженное напряжение, мВ; -1 — ещё не измерено */
bool hud_light_enabled(void);
void hud_light_set_enabled(bool on);
void hud_light_get_cal(int *dark_mv, int *bright_mv);
void hud_light_set_cal(int dark_mv, int bright_mv);
bool hud_light_cal_valid(int dark_mv, int bright_mv);   /* разница достаточна для калибровки */
int  hud_light_lux(void);                    /* «лк» для общей формулы яркости; -1 — нет данных */

#ifdef __cplusplus
}
#endif
