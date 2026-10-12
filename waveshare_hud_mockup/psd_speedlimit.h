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

/* Диагностика привязки к карте (HUD_PSD_DIAG): PSD_04 бит 37, PSD_06 мультиплекс 0 */
typedef struct {
    bool     pos_fresh;    /* PSD_05 свежий (позиция на сегменте известна)            */
    uint8_t  adas_q;       /* PSD_ADAS_Qualitaet текущего сегмента                     */
    bool     adas_fresh;   /* PSD_04 приходил недавно                                  */
    uint8_t  pos_err;      /* PSD_05 Pos_Fehler_Laengsrichtung (7 — Off-Road)          */
    uint8_t  mm_qual;      /* PSD_06 m0 Mapmatchingguete 0..3                          */
    uint8_t  geom_qual;    /* PSD_06 m0 Geometrieguete 0..3                            */
    uint8_t  sign_src;     /* PSD_06 m2 Verkehrszeichen_Quelle: 0/1 VZA, 2 VZO, 3 оба  */
    bool     sign_src_ok;  /* m2 приходил недавно                                      */
    uint8_t  cat_cur;      /* категория дороги сегмента, где машина (7 — неизвестно)  */
    uint8_t  urban_cur;    /* застроенная территория на текущем сегменте              */
    bool     m0_fresh;     /* PSD_06 mux 0 приходил недавно                            */
    uint8_t  country;      /* Laendercode (178 = RU, 20 = BY)                          */
    uint8_t  guidance;     /* Zielfuehrung — маршрут активен                           */
    uint8_t  qual_avail;   /* Quali_verfuegbar                                         */
    uint8_t  qual_tempo;   /* Quali_Tempolimits 0..7                                   */
    uint32_t n04, n04_q1, n04_cat7, n05, n05_off;   /* счётчики кадров PSD_04 за всё время           */
} psd_diag_t;

void        psd_reset(void);
void        psd_get_diag(uint32_t now_ms, psd_diag_t *out);
void        psd_on_frame(uint32_t id, const uint8_t *d, uint8_t dlc, uint32_t now_ms);
psd_limit_t psd_get(uint32_t now_ms, uint32_t pos_timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
