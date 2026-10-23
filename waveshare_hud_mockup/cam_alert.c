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

#ifndef CAM_REAR_AHEAD_S
#define CAM_REAR_AHEAD_S 6.0f       /* камера «в спину» (признак 1): окно, секунд хода (в лицо — ahead_s) */
#endif
#ifndef CAM_REAR_MIN_M
#define CAM_REAR_MIN_M   100.0f
#endif

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

/* Защита от ложных оповещений и поправка на задержку позиции */
#ifndef CAM_LEAD_S
#define CAM_LEAD_S      0.5f    /* позиция GNSS из шины отстаёт примерно на 0.6 с: экстраполируем вперёд */
#endif
#ifndef CAM_TURN_NEW_DEG
#define CAM_TURN_NEW_DEG 12.0f  /* курс за последние ~2 с изменился больше — новую камеру не показываем (поворот) */
#endif
#ifndef CAM_TURN_DROP_DEG
#define CAM_TURN_DROP_DEG 30.0f /* ...а показанную снимаем, если курс «уехал» больше этого */
#endif
#ifndef CAM_CONFIRM_MS
#define CAM_CONFIRM_MS  400     /* новая камера должна подтверждаться столько мс подряд (против мерцания) */
#endif
#ifndef CAM_HOLD_MS
#define CAM_HOLD_MS     1500    /* потеряли камеру на мгновение (шум курса на малой скорости) — держим ещё столько мс, пока она «впереди» */
#endif
#define HIST_N      12
static struct { uint32_t t; float h; } s_hist[HIST_N];
static int      s_hist_n, s_hist_w;
static uint32_t s_pend = 0xFFFFFFFFu, s_pend_t;
static cam_hit_t s_last; static uint32_t s_last_t;         /* последнее подтверждённое попадание */

static float adiff(float a, float b) { return fabsf(fmodf(a - b + 540.0f, 360.0f) - 180.0f); }

/* отклонение текущего курса от курса ~1.5..2.5 секунды назад */
static float turn_dev(uint32_t now, float hdg)
{
    float dev = 0; bool have = false;
    for (int i = 0; i < s_hist_n; i++) {
        uint32_t age = now - s_hist[i].t;
        if (age >= 1200 && age <= 3000) { float d = adiff(hdg, s_hist[i].h); if (!have || d > dev) dev = d; have = true; }
    }
    return have ? dev : 0.0f;
}

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

bool cam_query(uint32_t now_ms, double lat, double lon, float hdg_deg, float speed_kmh, cam_hit_t *out)
{
    if (!cam_loaded() || speed_kmh <= 0.5f) return false;

    /* история курса (не чаще раза в 250 мс) */
    if (s_hist_n == 0 || now_ms - s_hist[(s_hist_w + HIST_N - 1) % HIST_N].t >= 250) {
        s_hist[s_hist_w].t = now_ms; s_hist[s_hist_w].h = hdg_deg;
        s_hist_w = (s_hist_w + 1) % HIST_N; if (s_hist_n < HIST_N) s_hist_n++;
    }
    float dev = turn_dev(now_ms, hdg_deg);

    /* поправка на запаздывание позиции */
    {
        /* GNSS приходит ~1.25 Гц: добавляем время, прошедшее с последнего нового фикса */
        static double p_lat, p_lon; static uint32_t p_t;
        if (lat != p_lat || lon != p_lon) { p_lat = lat; p_lon = lon; p_t = now_ms; }
        float age_s = (float)(now_ms - p_t) / 1000.0f;
        if (age_s > 1.5f) age_s = 1.5f;
        double hr = hdg_deg * (M_PI / 180.0), dl = speed_kmh / 3.6 * (CAM_LEAD_S + age_s);
        lat += cos(hr) * dl / M_PER_DEG;
        lon += sin(hr) * dl / (M_PER_DEG * cos(lat * (M_PI / 180.0)));
    }

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
                if ((r->tf >> 4) == 1) {            /* снимает в спину: оповещаем позже */
                    float Rr = speed_kmh / 3.6f * CAM_REAR_AHEAD_S;
                    if (Rr < CAM_REAR_MIN_M) Rr = CAM_REAR_MIN_M;
                    if (Rr > R) Rr = R;
                    if (along > Rr) continue;
                }
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
    if (found) {
        if (b.idx != s_active) {                             /* новая камера: не в повороте и после подтверждения */
            if (dev > CAM_TURN_NEW_DEG) { s_pend = 0xFFFFFFFFu; return false; }
            if (b.idx != s_pend) { s_pend = b.idx; s_pend_t = now_ms; }
            if (now_ms - s_pend_t < CAM_CONFIRM_MS) return false;
        } else if (dev > CAM_TURN_DROP_DEG) {                /* поворот — камера уже не по пути */
            s_active = 0xFFFFFFFFu; s_pend = 0xFFFFFFFFu; return false;
        }
        s_active = b.idx; s_last = b; s_last_t = now_ms; if (out) *out = b;
        return true;
    }
    s_pend = 0xFFFFFFFFu;
    if (s_active != 0xFFFFFFFFu && now_ms - s_last_t <= CAM_HOLD_MS) {
        float dt = (now_ms - s_last_t) / 1000.0f, left = s_last.dist_m - speed_kmh / 3.6f * dt;
        if (left > 15.0f) {                                  /* камера ещё впереди — держим, расстояние уменьшаем */
            if (out) { *out = s_last; out->dist_m = left; }
            return true;
        }
    }
    s_active = 0xFFFFFFFFu;
    return false;
}
