/*
  hud_ota.h — обновление прошивки по Wi-Fi (OTA) через веб-страницу
  ------------------------------------------------------------------------------
  Удержите кнопку BOOT HUD_OTA_HOLD_MS (машина стоит) — HUD перезагружается в
  режим обновления: поднимает точку доступа Wi-Fi, на экране — имя сети, пароль
  и адрес страницы. Откройте страницу, выберите .bin (Скетч → Экспорт
  скомпилированного двоичного файла) — прошивка заливается и HUD перезагружается.
  В режиме обновления CAN, BLE и звук не запускаются.
*/
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { HUD_OTA_WAIT = 0, HUD_OTA_UPLOAD, HUD_OTA_DONE, HUD_OTA_ERROR };

typedef struct {
    int  state;              /* HUD_OTA_*                                  */
    int  percent;            /* 0..100 при загрузке                        */
    char ssid[24];
    char pass[24];
    char ip[20];
    char msg[48];            /* текст ошибки                               */
    int  left_s;             /* сколько секунд осталось до выхода          */
} HudOtaInfo;

/* В начале setup() (после hud_settings_load): прочитать и сбросить флаг
   «загрузиться в режиме обновления». true — режим обновления включён. */
bool hud_ota_check_boot(void);
bool hud_ota_mode(void);

/* Обычный режим: задача, следящая за кнопкой BOOT. */
void hud_ota_start_button(void);

/* Режим обновления: точка доступа + веб-сервер (своя задача). */
void hud_ota_run(void);

void hud_ota_get(HudOtaInfo *out);

#ifdef __cplusplus
}
#endif
