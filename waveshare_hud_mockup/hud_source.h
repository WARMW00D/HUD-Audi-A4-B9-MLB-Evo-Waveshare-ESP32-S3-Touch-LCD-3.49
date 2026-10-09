#ifndef HUD_SOURCE_H
#define HUD_SOURCE_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Запускает источник(и) CAN-кадров по HUD_DATA_SOURCE из hud_config.h:
   BLE-клиент сниффера, свой трансивер (TWAI), оба сразу (AUTO) или генератор. */
void hud_source_start(void);

/* ---- для модулей-источников ---- */
enum { HUD_SRC_ID_BLE = 0, HUD_SRC_ID_TWAI = 1, HUD_SRC_ID_FAKE = 2 };
void hud_src_set_up(int src, bool up);   /* связь источника есть / нет           */
void hud_src_twai_frame(void);           /* TWAI принял кадр (для выбора в AUTO) */
bool hud_src_twai_active(void);
int  hud_src_current(void);              /* -1 нет связи, 0 BLE (или генератор), 1 CAN */          /* по TWAI недавно были кадры           */
void hud_src_set_pair_failed(bool on);   /* BLE: сопряжение не удаётся (подсказка на экране) */
bool hud_src_pair_failed(void);

void ble_source_start(void);
void twai_source_start(void);
void fake_source_start(void);
#ifdef __cplusplus
}
#endif
#endif
