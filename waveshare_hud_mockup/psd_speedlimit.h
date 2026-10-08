/*
  psd_speedlimit.h — ограничения скорости из прогноза маршрута MIB (PSD)
  ------------------------------------------------------------------------------
  PSD_04 0x462 — сегменты впереди, PSD_05 0x463 — где машина, PSD_06 0x464
  (мультиплекс 2) — ограничения. Работает и тогда, когда VZE_01 молчит
  (старт «вне карты»). Раскладка — PSD_SpeedLimit_for_HUD.md (01.10.2026).
*/
#ifndef PSD_SPEEDLIMIT_H
#define PSD_SPEEDLIMIT_H
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PSD_LIM_NONE = 0,     /* нет данных                                   */
    PSD_LIM_EXPLICIT,     /* явный знак                                   */
    PSD_LIM_LEGAL,        /* по правилам (город / вне города / магистраль) */
    PSD_LIM_ENDED,        /* конец ограничения -> по правилам              */
} psd_src_t;

typedef struct {
    uint16_t  kmh;          /* текущее ограничение, 0 — нет данных */
    psd_src_t src;
    bool      no_overtake;  /* запрет обгона по явной записи */
} psd_limit_t;

void        psd_reset(void);
void        psd_on_frame(uint32_t id, const uint8_t *d, uint8_t dlc, uint32_t now_ms);
psd_limit_t psd_get(uint32_t now_ms, uint32_t pos_timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
