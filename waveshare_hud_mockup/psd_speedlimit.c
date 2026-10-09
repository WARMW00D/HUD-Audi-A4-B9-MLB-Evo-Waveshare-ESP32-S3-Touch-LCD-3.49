/*
  psd_speedlimit.c — ограничения скорости из PSD (см. psd_speedlimit.h)
  ------------------------------------------------------------------------------
  1. PSD_04 -> таблица сегментов (предыдущий, длина, категория, город).
  2. PSD_06 mux 2 -> явные ограничения (тип 1) по сегментам и смещениям;
     «по правилам» (тип 2) — отдельно по категориям город / вне / магистраль.
     Условные записи (прицеп, погода, дни, часы) и тип 0 пропускаются.
  3. PSD_05 -> текущий сегмент и позиция (длина - остаток). Действует последняя
     явная запись до нашей позиции; если в сегменте её нет — идём назад по
     цепочке «предыдущий сегмент».
  4. Нет явной -> по правилам: магистраль 110, город 60, иначе 90 (или что
     прислал MIB в записях типа 2).
  5. Цепочка порвалась (MIB перестроил дерево) — прежний явный знак держится 3 с.

  Всё на статических массивах, без malloc и без библиотечных блокировок:
  psd_on_frame() вызывается из задачи приёма CAN, psd_get() — из задачи экрана,
  общая защита — свой spinlock.
*/
#include "psd_speedlimit.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define NSEG       64
#define NREC       48
#define REC_TTL    30000u     /* запись без повторов дольше — устарела, мс */
#define HOLD_MS    3000u      /* держать явный знак при разрыве цепочки   */
#define WALK_MAX   16

typedef struct {
    bool     ok;
    uint8_t  prev;
    uint16_t len_m;
    uint8_t  cat;            /* 0..5, 5 — магистраль */
    uint8_t  urban;
} seg_t;

typedef struct {
    bool     ok;
    uint8_t  seg;
    uint16_t off_m;
    uint8_t  code;           /* код PSD_Ges_Geschwindigkeit */
    uint8_t  no_overtake;
    uint32_t t;
} rec_t;

static seg_t    s_seg[NSEG];
static rec_t    s_rec[NREC];
static uint16_t s_legal[4];          /* [1] город, [2] вне города, [3] магистраль, км/ч */
static uint8_t  s_pos_seg;
static uint16_t s_pos_remain;
static uint32_t s_pos_t;
static bool     s_pos_ok;
static psd_limit_t s_last_explicit;
static uint32_t s_last_explicit_t;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static inline uint32_t sg(const uint8_t *d, int start, int len)
{
    uint64_t raw = 0;
    for (int i = 7; i >= 0; i--) raw = (raw << 8) | d[i];
    return (uint32_t)((raw >> start) & ((1ULL << len) - 1));
}

/* код -> км/ч (нижняя граница диапазона); 0 — нет, 0xFFFF — конец ограничения */
static uint16_t code_kmh(uint8_t c)
{
    static const uint8_t t[23] = { 0, 5, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 60, 70, 80, 90, 100, 110,
                                   120, 130, 140, 150, 160 };
    if (c == 23) return 0xFFFF;
    return c < 23 ? t[c] : 0;
}

void psd_reset(void)
{
    taskENTER_CRITICAL(&s_mux);
    memset(s_seg, 0, sizeof s_seg); memset(s_rec, 0, sizeof s_rec); memset(s_legal, 0, sizeof s_legal);
    s_pos_ok = false; s_last_explicit.kmh = 0; s_last_explicit_t = 0;
    taskEXIT_CRITICAL(&s_mux);
}

void psd_on_frame(uint32_t id, const uint8_t *in, uint8_t dlc, uint32_t now)
{
    uint8_t d[8] = {0};
    memcpy(d, in, dlc > 8 ? 8 : dlc);
    taskENTER_CRITICAL(&s_mux);
    if (id == 0x462) {                                           /* PSD_04 */
        uint8_t sid = sg(d, 0, 6);
        if (sid >= 2) {
            seg_t n = { true, (uint8_t)sg(d, 6, 6), (uint16_t)(sg(d, 12, 7) * 2), (uint8_t)sg(d, 19, 3), (uint8_t)sg(d, 43, 1) };
            seg_t *o = &s_seg[sid];
            if (o->ok && (o->prev != n.prev || o->len_m != n.len_m))   /* ID занят другим сегментом */
                for (int k = 0; k < NREC; k++) if (s_rec[k].ok && s_rec[k].seg == sid) s_rec[k].ok = false;
            *o = n;
        }
    } else if (id == 0x463) {                                    /* PSD_05 */
        s_pos_seg = sg(d, 0, 6);
        s_pos_remain = (uint16_t)(sg(d, 6, 7) * 2);
        s_pos_t = now; s_pos_ok = s_pos_seg >= 2;
    } else if (id == 0x464 && sg(d, 0, 3) == 2) {                /* PSD_06 mux 2 */
        uint8_t typ = sg(d, 21, 2);
        bool cond = sg(d, 29, 2) || sg(d, 31, 2) || sg(d, 33, 3) || sg(d, 36, 3) ||
                    (sg(d, 39, 5) != 25 && sg(d, 39, 5) != 0) || (sg(d, 44, 5) != 25 && sg(d, 44, 5) != 0);
        uint8_t code = sg(d, 16, 5);
        if (typ == 2 && !cond) {
            uint8_t cat = sg(d, 56, 3);
            uint16_t k = code_kmh(code);
            if (cat >= 1 && cat <= 3 && k && k != 0xFFFF) s_legal[cat] = k;
        } else if (typ == 1 && !cond) {
            uint8_t sid = sg(d, 3, 6);
            uint16_t off = (uint16_t)(sg(d, 9, 7) * 2);
            int slot = -1, oldest = 0;
            for (int k = 0; k < NREC; k++) {
                if (s_rec[k].ok && s_rec[k].seg == sid && s_rec[k].off_m == off) { slot = k; break; }
                if (!s_rec[k].ok) { if (slot < 0) slot = k; }
                else if (s_rec[k].t < s_rec[oldest].t) oldest = k;
            }
            if (slot < 0) slot = oldest;
            s_rec[slot] = (rec_t){ true, sid, off, code, (uint8_t)(sg(d, 49, 2) == 1), now };
        }
    }
    taskEXIT_CRITICAL(&s_mux);
}

/* явная запись: в сегменте sid с offset <= maxoff (maxoff = 0xFFFF — любая), самая дальняя */
static const rec_t *find_rec(uint8_t sid, uint16_t maxoff, uint32_t now)
{
    const rec_t *best = NULL;
    for (int k = 0; k < NREC; k++) {
        const rec_t *r = &s_rec[k];
        if (!r->ok || r->seg != sid || r->off_m > maxoff || now - r->t > REC_TTL) continue;
        if (!best || r->off_m > best->off_m) best = r;
    }
    return best;
}

psd_limit_t psd_get(uint32_t now, uint32_t pos_timeout_ms)
{
    psd_limit_t L = { 0, PSD_LIM_NONE, false };
    taskENTER_CRITICAL(&s_mux);
    if (!s_pos_ok || now - s_pos_t > pos_timeout_ms) { taskEXIT_CRITICAL(&s_mux); return L; }

    uint8_t sid = s_pos_seg;
    const seg_t *S = &s_seg[sid];
    uint16_t pos = (S->ok && S->len_m > s_pos_remain) ? (uint16_t)(S->len_m - s_pos_remain) : 0;

    const rec_t *r = find_rec(sid, pos, now);
    uint8_t cur = sid;
    for (int w = 0; !r && w < WALK_MAX; w++) {                  /* назад по цепочке */
        if (!s_seg[cur].ok || s_seg[cur].prev < 2 || s_seg[cur].prev == cur) break;
        cur = s_seg[cur].prev;
        r = find_rec(cur, 0xFFFF, now);
    }

    if (r && code_kmh(r->code) && code_kmh(r->code) != 0xFFFF) {
        L.kmh = code_kmh(r->code); L.src = PSD_LIM_EXPLICIT; L.no_overtake = r->no_overtake;
        s_last_explicit = L; s_last_explicit_t = now;
    } else if (!r && s_last_explicit.kmh && now - s_last_explicit_t < HOLD_MS) {
        L = s_last_explicit;                                     /* цепочка порвалась — держим */
    } else {
        uint8_t cat = S->ok ? S->cat : 3;
        uint8_t k   = (cat == 5) ? 3 : (S->ok && S->urban) ? 1 : 2;
        static const uint16_t def[4] = { 0, 60, 90, 110 };       /* правила РФ, если MIB не прислал */
        L.kmh = s_legal[k] ? s_legal[k] : def[k];
        L.src = (r && code_kmh(r->code) == 0xFFFF) ? PSD_LIM_ENDED : PSD_LIM_LEGAL;
        L.no_overtake = false;
    }
    taskEXIT_CRITICAL(&s_mux);
    return L;
}
