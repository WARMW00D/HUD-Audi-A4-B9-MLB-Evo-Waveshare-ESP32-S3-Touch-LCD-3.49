/*
  cam_alert.c — см. cam_alert.h
  Формат cams.bin: "CAM1", u32 N, u32 дата, u32 резерв; затем N записей по 12 байт
  (f32 lat, f32 lon, u16 направление, u8 лимит, u8 тип | признак<<4), отсортированных
  по ключу (int(lat*20), int(lon*20)).
*/
#include "cam_alert.h"
#include <math.h>
#include <string.h>

#define CELL      20            /* ячеек на градус */
#define M_PER_DEG 111320.0

typedef struct __attribute__((packed)) {
    float    lat, lon;
    uint16_t dir;
    uint8_t  lim;
    uint8_t  tf;                /* тип (низ. 4 бита) | признак << 4 */
} rec_t;

static const rec_t *s_rec;
static uint32_t     s_n, s_date;
static cam_cfg_t    s_cfg = { 12.0f, 200.0f, 600.0f, 35.0f, 50.0f, 0x001 };
static uint32_t     s_active = 0xFFFFFFFFu;          /* камера, за которой уже следим (гистерезис) */

static inline uint32_t key_of(const rec_t *r)
{
    return (uint32_t)((int)(r->lat * CELL)) * 4096u + (uint32_t)((int)(r->lon * CELL));
}

static uint32_t lower_bound(uint32_t key)
{
    uint32_t lo = 0, hi = s_n;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        if (key_of(&s_rec[m]) < key) lo = m + 1; else hi = m;
    }
    return lo;
}

bool cam_load(const void *buf, size_t len)
{
    s_rec = NULL; s_n = 0;
    if (!buf || len < 16 || memcmp(buf, "CAM1", 4) != 0) return false;
    const uint32_t *h = (const uint32_t *)buf;
    uint32_t n = h[1];
    if ((size_t)16 + (size_t)n * sizeof(rec_t) != len) return false;
    s_date = h[2];
    s_rec = (const rec_t *)((const uint8_t *)buf + 16);
    s_n = n;
    s_active = 0xFFFFFFFFu;
    return true;
}

void     cam_unload(void)  { s_rec = NULL; s_n = 0; }
bool     cam_loaded(void)  { return s_rec != NULL && s_n > 0; }
uint32_t cam_count(void)   { return s_n; }
uint32_t cam_date(void)    { return s_date; }
void     cam_config(const cam_cfg_t *c) { if (c) s_cfg = *c; }

bool cam_query(double lat, double lon, float hdg_deg, float speed_kmh, cam_hit_t *out)
{
    if (!cam_loaded() || speed_kmh <= 0.5f) return false;

    float R = speed_kmh / 3.6f * s_cfg.ahead_s;
    if (R < s_cfg.min_m) R = s_cfg.min_m;
    if (R > s_cfg.max_m) R = s_cfg.max_m;

    double h  = hdg_deg * (M_PI / 180.0);
    double sh = sin(h), ch = cos(h);
    double kx = M_PER_DEG * cos(lat * (M_PI / 180.0));      /* м на градус долготы */

    int cy = (int)(lat * CELL), cx = (int)(lon * CELL);
    float best = 1e9f; cam_hit_t b; bool found = false;

    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            uint32_t key = (uint32_t)(cy + dy) * 4096u + (uint32_t)(cx + dx);
            for (uint32_t i = lower_bound(key); i < s_n && key_of(&s_rec[i]) == key; i++) {
                const rec_t *r = &s_rec[i];
                if (!((s_cfg.type_mask >> (r->tf & 15)) & 1)) continue;
                double ey = ((double)r->lat - lat) * M_PER_DEG;
                double ex = ((double)r->lon - lon) * kx;
                double along = ex * sh + ey * ch;
                double cross = ex * ch - ey * sh;
                if (cross < 0) cross = -cross;
                if (along <= 0 || along > R) continue;
                if (cross > (i == s_active ? s_cfg.cross_m * 1.7f : s_cfg.cross_m)) continue;
                if (r->dir != 0xFFFF) {
                    float d = fabsf(fmodf((float)r->dir - hdg_deg + 540.0f, 360.0f) - 180.0f);
                    if (d > s_cfg.dir_deg) continue;
                }
                if (along < best) {
                    best = (float)along;
                    b.dist_m = (float)along; b.cross_m = (float)cross; b.idx = i;
                    b.dir_deg = r->dir; b.limit = r->lim; b.type = r->tf & 15;
                    found = true;
                }
            }
        }
    }
    if (found) { s_active = b.idx; if (out) *out = b; }
    else s_active = 0xFFFFFFFFu;
    return found;
}
