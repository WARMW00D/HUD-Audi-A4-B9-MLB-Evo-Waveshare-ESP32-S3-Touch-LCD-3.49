/*
  hud_diag.h — отчёт о последнем сеансе для страницы режима обновления
  ------------------------------------------------------------------------------
  В обычном режиме HUD копит счётчики (g_diag). Перед перезагрузкой в режим
  обновления (удержание BOOT) снимок пишется во флеш (NVS), а страница портала
  показывает его в блоке «Диагностика». Живых данных в режиме обновления нет:
  CAN/BLE там остановлены — поэтому отчёт именно о прошлой поездке.
*/
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { DB_PENDING = 0, DB_OK, DB_NO_FATFS, DB_NO_FILE, DB_NO_MEM, DB_BAD };

typedef struct {
    uint32_t ver;                 /* версия структуры (HUD_DIAG_VER) */
    uint32_t uptime_s;
    uint32_t heap_free, psram_free;
    uint8_t  reset_reason;        /* esp_reset_reason() */
    int8_t   src;                 /* -1 нет связи, 0 BLE, 1 CAN */
    uint8_t  db_status;           /* DB_* */
    uint8_t  cams_on, cam_beep;
    uint32_t db_n, db_date;
    /* проверка условий показа камеры (каждый тик обновления экрана); причины взаимоисключающие, по порядку */
    uint32_t ticks;
    uint32_t no_cams_on, no_db, no_gps, no_hdg, no_spd, no_fix, no_sat, no_hdop, no_hdg_val, too_slow;
    uint32_t q_run;               /* запросов к базе */
    uint32_t q_hit;               /* из них — камера найдена */
    uint32_t shown;               /* показано разных камер */
    uint32_t last_idx; uint16_t last_lim, last_dist_m, last_spd;
    int32_t  lat_e6, lon_e6;      /* последняя позиция, когда условия выполнялись */
    uint16_t max_spd;
} hud_diag_t;

#define HUD_DIAG_VER 1

extern hud_diag_t g_diag;         /* счётчики текущего сеанса (пишет hud_mockup.c, hud_cams.cpp) */

void hud_diag_save(void);                 /* снимок во флеш; вызывать перед перезагрузкой в режим обновления */
bool hud_diag_load(hud_diag_t *out);      /* прочитать сохранённый отчёт; false — нет */

#ifdef __cplusplus
}
#endif
