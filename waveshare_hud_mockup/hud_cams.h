/*
  hud_cams.h — база камер (cams.bin) во флеше (FATFS) -> PSRAM
  ------------------------------------------------------------------------------
  Загрузка идёт в фоновой задаче при старте: монтирует FATFS (раздел "ffat"),
  читает /cams.bin в PSRAM и передаёт его в cam_alert. Если файла или раздела нет —
  камеры просто не работают. Заливка файла — со страницы режима обновления (hud_ota).
*/
#ifndef HUD_CAMS_H
#define HUD_CAMS_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void hud_cams_start(void);     /* фоновая загрузка базы */
bool hud_cams_ready(void);     /* база загружена и годна */
#ifdef __cplusplus
}
#endif
#endif
