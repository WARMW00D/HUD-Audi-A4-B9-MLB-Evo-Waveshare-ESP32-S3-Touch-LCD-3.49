/*
  cam_alert.h — камеры фиксации скорости по позиции и курсу GNSS
  ------------------------------------------------------------------------------
  База cams.bin (tools/gen_cams.py) целиком лежит в памяти (PSRAM), записи
  отсортированы по ячейке 0.05°. cam_query() берёт позицию, курс и скорость и
  возвращает ближайшую камеру впереди по ходу движения: в пределах окна по
  расстоянию (зависит от скорости), недалеко от оси движения и (если у камеры
  задано направление) с совпадающим направлением. Чистый C, без зависимостей от
  ESP — проверяется на ПК (см. tools/cam_replay.c).
*/
#ifndef CAM_ALERT_H
#define CAM_ALERT_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float    dist_m;     /* расстояние вдоль курса до камеры          */
    float    cross_m;    /* смещение от оси движения                  */
    uint32_t idx;        /* номер записи в базе                       */
    uint16_t dir_deg;    /* направление контролируемого движения, 0xFFFF — нет */
    uint8_t  limit;      /* ограничение, км/ч (0 — нет данных)        */
    uint8_t  type;       /* индекс типа 0..8 (см. gen_cams.py)        */
} cam_hit_t;

typedef struct {
    float    ahead_s;    /* окно оповещения: секунд хода                       */
    float    min_m;      /* ...но не меньше, м                                  */
    float    max_m;      /* ...и не больше, м                                   */
    float    cross_m;    /* допустимое смещение от оси, м                       */
    float    dir_deg;    /* допуск по направлению камеры, °                     */
    uint16_t type_mask;  /* какие типы камер учитывать (бит = индекс типа)      */
} cam_cfg_t;

/* buf должен жить, пока база используется. false — формат не тот / размер не сходится */
bool     cam_load(const void *buf, size_t len);
void     cam_unload(void);
bool     cam_loaded(void);
uint32_t cam_count(void);
uint32_t cam_date(void);                 /* дата базы ддммгг числом */
void     cam_config(const cam_cfg_t *c);

/* Ближайшая камера по ходу. now_ms — монотонное время, мс; hdg_deg — курс 0..360, speed_kmh > 0.
   Внутри: поправка позиции на запаздывание GNSS, подавление оповещений в поворотах и
   подтверждение новой камеры ~0.4 с (против мерцания на малой скорости). */
bool     cam_query(uint32_t now_ms, double lat, double lon, float hdg_deg, float speed_kmh, cam_hit_t *out);

#ifdef __cplusplus
}
#endif
#endif
