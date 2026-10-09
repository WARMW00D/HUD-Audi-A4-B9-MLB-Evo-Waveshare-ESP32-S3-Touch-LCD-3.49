/*
  can_decode.c — декодер кадров I-CAN (MLB-Evo) в HudData
  ------------------------------------------------------------------------------
  Источник позиций сигналов: K-матрица MLB-Evo I-CAN V8.21 (см. CAN_to_HUD_handoff.md).
  Все 11-битные сигналы Intel (little-endian).
  BAP Navigation_SD (LSG 0x32): 29-bit 0x17333210 / 0x17333211, сборка
  многокадровых сообщений по ключу (ID, канал).
*/
#include "hud_data.h"
#include "hud_config.h"
#include "psd_speedlimit.h"
#include "hud_log.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

/* ---------- таймауты свежести (мс) ---------- */
#define TO_SPEED   1000
#define TO_ACC     1000
#define TO_GEAR    1000
#define TO_BLINK   2500
#define TO_SIGN    1500
#define TO_LKA     1500
#define TO_NAV     60000  /* BAP шлёт Status при изменении, heartbeat ~25 с */
#define TO_DOORS   1500   /* ZV_02: 200 мс, в ACL не реже 1 с   */
#define TO_HOOD    3000   /* BCM_01: 1000 мс                    */
#define TO_GNUM    1000
#define TO_JAM     1000
#define TO_LIM     1000
#define TO_AWV     600
#define TO_DIM     3000
#define TO_ACCEL   500
#define TO_SPDACC  600
#define TO_BRAKE   500
#define TO_FUEL    3000
#define TO_CLOCK   5000
#define TO_BC      60000
#define TO_RLS     1500
#define TO_SWA     1000
#define TO_WHEEL   3000    /* BCM1_04: раз в 1 с в покое, 50-60 мс при вращении */   /* BAP_BC: 0x18 раз в ~4 с при изменении, остальное — heartbeat ~25 с */

enum { S_SPEED, S_ACC, S_GEAR, S_BLINK, S_SIGN, S_MAN, S_MDIST, S_DEST, S_TTE, S_LKA, S_DOORS, S_HOOD, S_GNUM, S_JAM, S_LIM, S_AWV, S_DIM, S_ACCEL, S_SPDACC, S_BRAKE, S_FUEL, S_CLOCK, S_BC, S_BCFUEL, S_RLS, S_WHEEL, S_SWA, S_COUNT };
static const uint32_t s_timeout[S_COUNT] = {
    TO_SPEED, TO_ACC, TO_GEAR, TO_BLINK, TO_SIGN, TO_NAV, TO_NAV, TO_NAV, TO_NAV, TO_LKA, TO_DOORS, TO_HOOD, TO_GNUM, TO_JAM, TO_LIM, TO_AWV, TO_DIM, TO_ACCEL, TO_SPDACC, TO_BRAKE, TO_FUEL, TO_CLOCK, TO_BC, TO_BC, TO_RLS, TO_WHEEL, TO_SWA
};

static HudData      s_data;
static uint32_t     s_last[S_COUNT];     /* 0 = никогда / сброшено */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

uint32_t hud_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static uint32_t     s_gap_max[S_COUNT];  /* макс. пауза между кадрами, мс (диагностика) */

static inline void touch(int slot)
{
    uint32_t t = hud_now_ms();
    if (!t) t = 1;
    if (s_last[slot]) {
        uint32_t gap = t - s_last[slot];
        if ((int32_t)gap > 0 && gap > s_gap_max[slot]) s_gap_max[slot] = gap;
    }
    s_last[slot] = t;
}

/* Максимальная пауза между кадрами скорости с прошлого вызова (и сброс) */
uint32_t hud_data_take_speed_gap_max(void)
{
    taskENTER_CRITICAL(&s_mux);
    uint32_t g = s_gap_max[S_SPEED];
    s_gap_max[S_SPEED] = 0;
    taskEXIT_CRITICAL(&s_mux);
    return g;
}

void hud_data_snapshot(HudData *out)
{
    taskENTER_CRITICAL(&s_mux);
    /* now берётся ВНУТРИ критической секции: иначе кадр, пришедший на другом
       ядре между чтением времени и входом в секцию, даёт s_last > now,
       разность уходит в 0xFFFFxxxx и поле на один тик гаснет. */
    uint32_t now = hud_now_ms();
    *out = s_data;
    uint32_t v = 0;
    for (int i = 0; i < S_COUNT; i++)
        if (s_last[i] && (int32_t)(now - s_last[i]) < (int32_t)s_timeout[i]) v |= (1u << i);
    taskEXIT_CRITICAL(&s_mux);
    out->valid = v;
}

void hud_data_set_link(bool up)
{
    taskENTER_CRITICAL(&s_mux);
    s_data.link_up = up;
    taskEXIT_CRITICAL(&s_mux);
}

void hud_data_note_lost(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_data.batches_lost++;
    taskEXIT_CRITICAL(&s_mux);
}

/* ---------- извлечение сигнала Intel ---------- */
static inline uint32_t sig(const uint8_t *d, int start, int len)
{
    uint64_t raw = 0;
    for (int i = 7; i >= 0; i--) raw = (raw << 8) | d[i];
    return (uint32_t)((raw >> start) & ((1ULL << len) - 1));
}

static inline uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---------- Lane Assist: LDW_02 (0x397) ----------
   Раскладка — opendbc vw_mlb.dbc, смысл Lernmodus — openpilot.
   Биты 12-15 (LDW_Gong, LDW_SW_Warnung_*) НЕ использовать: в этой машине
   байт 1 постоянно 0x40. ⚠️ смысл значений в движении ещё не проверен. */
static void decode_lka(const uint8_t *d)
{
    bool green = sig(d, 62, 1);
    bool yellow = sig(d, 61, 1);
    uint8_t warn = (sig(d, 56, 1) ? 1 : 0) | (sig(d, 57, 1) ? 2 : 0);
    uint8_t ll = sig(d, 38, 2), lr = sig(d, 36, 2);

    s_data.lka_line_l = ll;
    s_data.lka_line_r = lr;
    s_data.lka_warn   = warn;
    /* Лог 29.09 (стоянка): при включении LKA индикаторы 61/62 НЕ меняются,
       а линии переходят 0 -> 1 ("не видна"). Поэтому "включён" = любая
       линия не 0; индикаторы уточняют состояние, когда они есть. */
    s_data.lka_state  = warn   ? LKA_INTERVENE :
                        green  ? LKA_ACTIVE    :
                        (yellow || ll || lr) ? LKA_PASSIVE : LKA_OFF;
}

/* ---------- Двери, багажник, капот ----------
   ZV_02 0x583 (200 мс): двери и крышка багажника.
   BCM_01 0x65A (1000 мс): концевик капота.
   Водительская дверь подтверждена логом 28.09; остальное — по K-матрице. */
static void decode_zv02(const uint8_t *d)
{
    uint8_t m = s_data.doors & DOOR_HOOD;            /* капот — из другого кадра */
    if (sig(d, 24, 1)) m |= DOOR_FL;                 /* ZV_FT_offen   */
    if (sig(d, 25, 1)) m |= DOOR_FR;                 /* ZV_BT_offen   */
    if (sig(d, 26, 1)) m |= DOOR_RL;                 /* ZV_HFS_offen  */
    if (sig(d, 27, 1)) m |= DOOR_RR;                 /* ZV_HBFS_offen */
    if (sig(d, 28, 1)) m |= DOOR_TRUNK;              /* ZV_HD_offen   */
    s_data.doors = m;
}

static void decode_bcm01(const uint8_t *d)
{
    s_data.doors = (s_data.doors & ~DOOR_HOOD) | (sig(d, 31, 1) ? DOOR_HOOD : 0);  /* BCM1_MH_Schalter */
}

/* ---------- ускорение по скорости (Kombi_01.KBI_angez_Geschw, 0.32 км/ч) ----------
   Запасной источник ускорения, если ESP_02 на I-CAN нет. Наклон прямой по
   методу наименьших квадратов через точки скорости за последние ~1.2 с:
   ступеньки 0.32 км/ч усредняются, а не дают скачков, как разность двух точек. */
#define SPD_HIST 16
static uint32_t s_spd_t[SPD_HIST];
static uint16_t s_spd_v[SPD_HIST];        /* км/ч x100 */
static uint8_t  s_spd_n, s_spd_i;

static void spd_accel_update(uint16_t v_x100)
{
    uint32_t t = hud_now_ms();
    s_spd_t[s_spd_i] = t; s_spd_v[s_spd_i] = v_x100;
    s_spd_i = (s_spd_i + 1) % SPD_HIST;
    if (s_spd_n < SPD_HIST) s_spd_n++;

    /* МНК по точкам не старше 1.2 с */
    float sx = 0, sy = 0, sxx = 0, sxy = 0; int n = 0;
    for (int k = 0; k < s_spd_n; k++) {
        uint32_t age = t - s_spd_t[k];
        if (age > 1200) continue;
        float x = -(float)age / 1000.0f;                  /* с, 0 — сейчас */
        float y = s_spd_v[k] / 100.0f / 3.6f;             /* м/с */
        sx += x; sy += y; sxx += x * x; sxy += x * y; n++;
    }
    if (n < 6) return;
    float den = n * sxx - sx * sx;
    if (den < 1e-6f) return;
    float a = (n * sxy - sx * sy) / den;                  /* м/с² */
    s_data.accel_spd_x100 = (int16_t)(a * 100.0f);
    touch(S_SPDACC);
}

/* ---------- мгновенный расход по счётчику топлива Motor_04.MO_KVS ----------
   MO_KVS — накопительный счётчик, мкл, 15 бит (переполняется на 32768).
   Раз в >= 1 с: л/ч = dмкл / dt * 3600 / 1e6, л/100 км = л/ч / км/ч * 100. */
static uint32_t s_kvs_t;
static uint16_t s_kvs_last;
static uint32_t s_kvs_acc;                 /* мкл с прошлого расчёта */
static bool     s_kvs_have;
static float    s_fuel_f;

static double s_avg_fuel_l;               /* топливо с запуска, л */
static double s_avg_dist_km;             /* путь с запуска, км (интеграл скорости) */

static void fuel_update(uint16_t kvs)
{
    uint32_t t = hud_now_ms();
    if (!s_kvs_have) { s_kvs_have = true; s_kvs_last = kvs; s_kvs_t = t; s_kvs_acc = 0;
                       s_data.fuel_avg_x10 = 0xFFFF; return; }
    s_kvs_acc += (uint16_t)((kvs - s_kvs_last) & 0x7FFF);
    s_kvs_last = kvs;
    uint32_t dt = t - s_kvs_t;
    if (dt < 1000) return;
    float lph = s_kvs_acc * 3.6f / (float)dt;                 /* мкл/мс * 3600/1000 = л/ч */
    float kmh = s_data.speed_kmh;

    /* средний с момента запуска: всё топливо (и на холостых) / весь путь */
    s_avg_fuel_l  += s_kvs_acc / 1e6;
    s_avg_dist_km += kmh * (dt / 3600000.0);
    if (s_avg_dist_km >= 0.5) {                               /* меньше полукилометра — рано */
        double avg = s_avg_fuel_l / s_avg_dist_km * 100.0 * 10.0;
        s_data.fuel_avg_x10 = (uint16_t)(avg > 9999 ? 9999 : avg);
    }

    s_kvs_acc = 0; s_kvs_t = t;
    if (kmh < 3) { s_fuel_f = 0; s_data.fuel_l100_x10 = 0; touch(S_FUEL); return; }
    float l100 = lph / kmh * 100.0f;
    s_fuel_f += (l100 - s_fuel_f) * 0.5f;                       /* сглаживание ~2 с */
    float v = s_fuel_f * 10.0f;
    s_data.fuel_l100_x10 = (uint16_t)(v > 9999 ? 9999 : v);
    touch(S_FUEL);
}

/* ---------- часы машины: Diagnose_01 (0x6B2) ----------
   UH_Jahr 28|7 (+2000), UH_Monat 35|4, UH_Tag 39|5, UH_Stunde 44|5,
   UH_Minute 49|6, UH_Sekunde 55|6 (opendbc MLB/MQB). Кадр раз в секунду;
   между кадрами время продолжаем по millis(). Это время приборки (местное).
   ВАЖНО: декодер работает внутри критической секции (spinlock) — никаких
   mktime()/localtime(): они берут мьютексы newlib, и ESP32 падает в abort().
   Поэтому дата <-> число дней считаются своей целочисленной арифметикой. */
static int32_t  s_clk_base;          /* секунды от 2000-01-01 00:00:00 */
static uint32_t s_clk_rx_ms;
static bool     s_clk_ok;

/* дни от 2000-01-01 (алгоритм days_from_civil, H. Hinnant) */
static int32_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 730425;        /* 730425 = дни 0000-03-01 .. 2000-01-01 */
}

static void civil_from_days(int32_t z, int *y, unsigned *m, unsigned *d)
{
    z += 730425;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int yy = (int)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yy + (*m <= 2);
}

static void decode_clock(const uint8_t *d)
{
    int y = 2000 + (int)sig(d, 28, 7);
    unsigned mo = sig(d, 35, 4), dd = sig(d, 39, 5), h = sig(d, 44, 5), mi = sig(d, 49, 6), se = sig(d, 55, 6);
    if (y < 2020 || mo < 1 || mo > 12 || dd < 1 || dd > 31 || h > 23 || mi > 59 || se > 59)
        return;                                             /* часы не выставлены / мусор */
    s_clk_base  = days_from_civil(y, mo, dd) * 86400 + (int32_t)(h * 3600 + mi * 60 + se);
    s_clk_rx_ms = hud_now_ms();
    s_clk_ok    = true;
    touch(S_CLOCK);
}

bool hud_clock_now(HudClock *c)
{
    taskENTER_CRITICAL(&s_mux);
    bool ok = s_clk_ok;
    int32_t base = s_clk_base;
    uint32_t el = hud_now_ms() - s_clk_rx_ms;
    taskEXIT_CRITICAL(&s_mux);
    if (!ok) return false;
    int32_t t = base + (int32_t)(el / 1000);
    int32_t days = t / 86400, sod = t % 86400;
    int y; unsigned m, dd;
    civil_from_days(days, &y, &m, &dd);
    c->year = (uint16_t)y; c->mon = (uint8_t)m; c->day = (uint8_t)dd;
    c->hour = (uint8_t)(sod / 3600); c->min = (uint8_t)(sod / 60 % 60); c->sec = (uint8_t)(sod % 60);
    c->ms = (uint16_t)(el % 1000);
    return true;
}

/* ---------- BAP Navigation_SD ---------- */
#define BAP_LSG_NAV   0x32
#define BAP_BUF_SIZE  256

typedef struct {
    bool     busy;
    uint16_t hdr;
    uint16_t need, len;
    uint8_t  seq;
    uint8_t  buf[BAP_BUF_SIZE];
} BapAsm;

static BapAsm s_bap[3][4];   /* [0x17333210 / 0x17333211 / 0x17330F10][канал] */

/* ---------- BAP_BC: бортовой компьютер приборки (LSG 0x0F) ----------
   Каталога функций нет — раскладка по поездкам 0006/0010 (сверено с одометром,
   баком, скоростью). Все поля little-endian. */
#define BAP_LSG_BC 0x0F
static inline uint16_t le16(const uint8_t *q) { return (uint16_t)(q[0] | (q[1] << 8)); }

static void bc_message(uint8_t fct, const uint8_t *p, uint16_t n)
{
    taskENTER_CRITICAL(&s_mux);
    switch (fct) {
    case 0x18:                                     /* с момента запуска */
    case 0x19:                                     /* долговременная память */
        if (n >= 17) {
            if (fct == 0x18) {
                s_data.bc_avg_x10  = le16(p);       s_data.bc_dist_x10 = le16(p + 6);
                s_data.bc_time_min = p[11];         s_data.bc_vavg_x10 = le16(p + 15);
                touch(S_BC);
            } else {
                s_data.bcl_avg_x10  = le16(p);      s_data.bcl_dist_x10 = le16(p + 6);
                s_data.bcl_time_min = p[11];        s_data.bcl_vavg_x10 = le16(p + 15);
            }
        }
        break;
    case 0x16: if (n >= 2) s_data.bc_range_km = le16(p); break;
    case 0x17: if (n >= 4) s_data.bc_odo_x10  = le32(p); break;
    case 0x1C: if (n >= 1 && p[0] <= 100) { s_data.bc_fuel_pct = p[0]; touch(S_BCFUEL); } break;
    default: break;
    }
    taskEXIT_CRITICAL(&s_mux);
}

static void bap_message(uint16_t hdr, const uint8_t *p, uint16_t n)
{
    uint8_t opcode = (hdr >> 12) & 7;
    uint8_t lsg    = (hdr >> 6) & 0x3F;
    uint8_t fct    = hdr & 0x3F;
    if (opcode != 3 && opcode != 4) return;   /* HeartbeatStatus / Status */
    if (lsg == BAP_LSG_BC) { bc_message(fct, p, n); return; }
    if (lsg != BAP_LSG_NAV) return;

#if HUD_LOG_BAP
    /* журнал остальных функций навигации (не разобранных): печать при смене содержимого.
       Нужен, чтобы найти, где приборка берёт данные для кольца (номер съезда и т.п.) */
    if (fct != 0x11 && fct != 0x12 && fct != 0x15 && fct != 0x16 && fct != 0x17) {
        static uint8_t  lg_f[64][24];
        static uint8_t  lg_n[64];
        uint8_t m = n > 24 ? 24 : (uint8_t)n;
        if (lg_n[fct] != m + 1 || memcmp(lg_f[fct], p, m) != 0) {
            lg_n[fct] = m + 1; memcpy(lg_f[fct], p, m);
            char hex[3 * 24 + 1] = ""; int o = 0;
            for (int k = 0; k < m; k++) o += snprintf(hex + o, sizeof hex - o, "%02X ", p[k]);
            arduino_printf("[bap] Navigation_SD op %u fct 0x%02X [%u]: %s\n", opcode, fct, (unsigned)n, hex);
        }
    }
#endif
    taskENTER_CRITICAL(&s_mux);
    switch (fct) {
    case 0x11: /* RG_Status */
        if (n >= 1) { s_data.rg_active = p[0]; touch(S_MAN); }
        break;
    case 0x12: /* DistanceToNextManeuver */
        if (n >= 8) {
            if (p[7] & 0x01) {
                s_data.man_dist_x10 = le32(p);
                s_data.man_unit     = p[4];
                s_data.bargraph     = p[5] ? (p[6] > 100 ? 100 : p[6]) : 0xFF;
                touch(S_MDIST);
            } else {
                s_last[S_MDIST] = 0;
            }
        }
        break;
    case 0x15: /* DistanceToDestination */
        if (n >= 6) {
            if (p[5] & 0x01) {
                s_data.dest_dist_x10 = le32(p);
                s_data.dest_unit     = p[4];
                touch(S_DEST);
            } else {
                s_last[S_DEST] = 0;
            }
        }
        break;
    case 0x16: /* TimeToDestination */
        if (n >= 7) {
            if ((p[6] & 0x06) == 0x06) {
                s_data.tte_type = p[0] >> 4;
                s_data.tte_min  = p[1];
                s_data.tte_h    = p[2];
                touch(S_TTE);
            } else {
                s_last[S_TTE] = 0;
            }
        }
        break;
    case 0x17: /* ManeuverDescriptor — только Maneuver_1 */
        if (n >= 2) {
            s_data.man_main = p[0];
            s_data.man_dir  = p[1];
            s_data.man_z    = n >= 3 ? p[2] : 0;
            uint8_t cnt = n >= 4 ? p[3] : 0;
            if (cnt > n - 4) cnt = (uint8_t)(n - 4);
            s_data.man_side_n = cnt;
            memset(s_data.man_side, 0, sizeof s_data.man_side);
            memcpy(s_data.man_side, p + 4, cnt < 8 ? cnt : 8);
            /* Maneuver_2: сразу за первым (4 байта + его боковые улицы) */
            {
                uint16_t off = (uint16_t)(4 + (n >= 4 ? p[3] : 0));
                s_data.man2_main = (off + 1 < n) ? p[off]     : 0;
                s_data.man2_dir  = (off + 1 < n) ? p[off + 1] : 0;
            }
            s_data.man_raw_len = (uint8_t)(n > 255 ? 255 : n);
            memset(s_data.man_raw, 0, sizeof s_data.man_raw);
            memcpy(s_data.man_raw, p, n < sizeof s_data.man_raw ? n : sizeof s_data.man_raw);
            touch(S_MAN);
        }
        break;
    default:
        break;
    }
    taskEXIT_CRITICAL(&s_mux);
}

static void bap_frame(int idx, const uint8_t *d, uint8_t dlc)
{
    if (dlc < 2) return;

    if (!(d[0] & 0x80)) {                               /* одиночный */
        bap_message(((uint16_t)d[0] << 8) | d[1], d + 2, dlc - 2);
        return;
    }

    BapAsm *a = &s_bap[idx][(d[0] >> 4) & 3];

    if (!(d[0] & 0x40)) {                               /* старт 10cc LLLL */
        if (dlc < 4) { a->busy = false; return; }
        a->need = ((uint16_t)(d[0] & 0x0F) << 8) | d[1];
        a->hdr  = ((uint16_t)d[2] << 8) | d[3];
        a->len  = 0;
        a->seq  = 0;
        a->busy = a->need > 0;
        uint16_t c = dlc - 4;
        if (c > a->need) c = a->need;
        if (a->need <= BAP_BUF_SIZE) memcpy(a->buf, d + 4, c);
        a->len = c;
    } else {                                            /* продолжение 11cc SSSS */
        if (!a->busy) return;
        if ((d[0] & 0x0F) != (a->seq & 0x0F)) { a->busy = false; return; }
        a->seq++;
        uint16_t c = dlc - 1;
        if (a->len + c > a->need) c = a->need - a->len;
        if (a->need <= BAP_BUF_SIZE) memcpy(a->buf + a->len, d + 1, c);
        a->len += c;
    }

    if (a->busy && a->len >= a->need) {
        a->busy = false;
        if (a->need <= BAP_BUF_SIZE)         /* длинные (TurnToInfo и т.п.) пропускаем */
            bap_message(a->hdr, a->buf, a->need);
    }
}

/* Для генератора (HUD_SRC_FAKE): прямая установка дверей */
void hud_data_fake_doors(uint8_t mask)
{
    taskENTER_CRITICAL(&s_mux);
    s_data.doors = mask;
    touch(S_DOORS);
    touch(S_HOOD);
    taskEXIT_CRITICAL(&s_mux);
}

/* ---------- диагностика: счётчики кадров по ID ---------- */
static const uint16_t s_dbg_ids[] = { 0x30B, 0x2A6, 0x2A8, 0x31E, 0x394, 0x366, 0x181, 0x397, 0x583, 0x65A, 0x2A9, 0x5F0, 0x5A0, 0x64F, 0x30F, 0x101, 0x6B2, 0x462, 0x463, 0x464 };
#define DBG_N (sizeof(s_dbg_ids) / sizeof(s_dbg_ids[0]))
static uint32_t s_dbg_cnt[DBG_N];
static uint8_t  s_dbg_last[DBG_N][8];
static uint32_t s_dbg_bap;

static void dbg_note(uint32_t id, bool ext, const uint8_t *d, uint8_t dlc)
{
    if (ext) { s_dbg_bap++; return; }
    for (unsigned i = 0; i < DBG_N; i++)
        if (s_dbg_ids[i] == id) {
            s_dbg_cnt[i]++;
            memset(s_dbg_last[i], 0, 8);
            memcpy(s_dbg_last[i], d, dlc);
            return;
        }
}

/* Печать: сколько кадров каждого ID пришло с прошлого вызова и последние данные.
   ID с нулём — кадр не приходит (нет в логе / на шине / отфильтрован ACL). */
void hud_data_debug_print(void (*out)(const char *line))
{
    char line[96];
    for (unsigned i = 0; i < DBG_N; i++) {
        uint32_t c; uint8_t b[8];
        taskENTER_CRITICAL(&s_mux);
        c = s_dbg_cnt[i]; s_dbg_cnt[i] = 0; memcpy(b, s_dbg_last[i], 8);
        taskEXIT_CRITICAL(&s_mux);
        snprintf(line, sizeof line, "[CAN] 0x%03X  %4lu кадров  %02X %02X %02X %02X %02X %02X %02X %02X\n",
                 s_dbg_ids[i], (unsigned long)c, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
        out(line);
    }
    taskENTER_CRITICAL(&s_mux);
    uint32_t bap = s_dbg_bap; s_dbg_bap = 0;
    taskEXIT_CRITICAL(&s_mux);
    snprintf(line, sizeof line, "[CAN] BAP  %4lu кадров\n", (unsigned long)bap);
    out(line);
}

/* ---------- вход ---------- */
void can_decode_frame(uint32_t id, bool ext, const uint8_t *data, uint8_t dlc)
{
    if (dlc > 8) return;

    taskENTER_CRITICAL(&s_mux);
    s_data.frames_rx++;
    dbg_note(id, ext, data, dlc);
    taskEXIT_CRITICAL(&s_mux);

    if (ext) {
        if (id == 0x17333210) bap_frame(0, data, dlc);
        else if (id == 0x17333211) bap_frame(1, data, dlc);
        else if (id == 0x17330F10) bap_frame(2, data, dlc);   /* BAP_BC от приборки */
        return;
    }

    /* PSD (прогноз маршрута MIB) — свой модуль со своей блокировкой */
    if (id >= 0x462 && id <= 0x464) {
        psd_on_frame(id, data, dlc, hud_now_ms());
        return;
    }

    uint8_t d[8] = {0};
    memcpy(d, data, dlc);

    taskENTER_CRITICAL(&s_mux);
    switch (id) {
    case 0x30B: /* Kombi_01 */
        s_data.speed_kmh = sig(d, 24, 9);
        touch(S_SPEED);
        spd_accel_update((uint16_t)(sig(d, 48, 10) * 32));     /* KBI_angez_Geschw x0.32 -> км/ч x100 */
        break;
    case 0x101: /* ESP_02 — ⚠️ из opendbc MLB, на I-CAN не проверено */
        s_data.accel_esp_x100 = (int16_t)(((int)sig(d, 24, 10) * 3125 - 1600000) / 1000);   /* x0.03125 - 16 */
        if (!sig(d, 13, 1)) touch(S_ACCEL);                       /* ESP_QBit_Laengsbeschl = 0 — значение годно */
        break;
    case 0x106: /* ESP_05 — ⚠️ из opendbc MLB, на I-CAN не проверено */
        s_data.brake_bar   = (int16_t)(((int)sig(d, 16, 10) * 3 - 300) / 10);   /* x0.3 - 30 */
        s_data.brake_pedal = sig(d, 26, 1);
        touch(S_BRAKE);
        break;
    case 0x107: /* Motor_04 — ⚠️ из opendbc MLB, на I-CAN не проверено */
        fuel_update((uint16_t)sig(d, 48, 15));
        break;
    case 0x2A6: /* ACC_12 */
        s_data.acc_set_raw    = sig(d, 12, 10);
        s_data.acc_tempolimit = sig(d, 0, 5);
        s_data.acc_gap        = sig(d, 37, 3);
        s_data.acc_object     = sig(d, 45, 2);
        s_data.aca_lane       = sig(d, 7, 2);
        s_data.jam_state      = sig(d, 62, 2);   /* STA_Primaeranz */
        touch(S_ACC);
        touch(S_JAM);
        break;
    case 0x2A8: /* ACC_14 */
        s_data.acc_status = sig(d, 16, 3);
        touch(S_ACC);
        break;
    case 0x394: /* WBA_03 */
        s_data.gear = sig(d, 12, 4);
        touch(S_GEAR);
        /* WBA_eing_Gang_02 24|4 — включённая передача (раскладка WBA_03 как в
           MQB opendbc; на логе 29.09 в D на месте = 1). ⚠️ 8-я и выше не проверены. */
        {
            uint8_t g = sig(d, 24, 4);
            s_data.gear_num = (g >= 1 && g <= 9) ? g : 0;
            touch(S_GNUM);
        }
        break;
    case 0x366: /* Blinkmodi_02 */
        s_data.blink = (sig(d, 27, 1) ? BLINK_L_TAKT : 0)
                     | (sig(d, 28, 1) ? BLINK_R_TAKT : 0)
                     | (sig(d, 20, 1) ? BLINK_HAZARD : 0);
        touch(S_BLINK);
        break;
    case 0x181: /* VZE_01 */
        s_data.sign_raw = sig(d, 11, 8);
        s_data.sign_mph      = sig(d, 8, 1);
        s_data.sign_suppress = sig(d, 50, 1);
        s_data.sign_warn     = sig(d, 35, 1);
        s_data.sign_raw2     = sig(d, 19, 8);
        s_data.sign_raw3     = sig(d, 27, 8);
        s_data.sign_sup2     = sig(d, 10, 1);
        s_data.sign_sup3     = sig(d, 9, 1);
        touch(S_SIGN);
        break;
    case 0x397: /* LDW_02 */
        decode_lka(d);
        touch(S_LKA);
        break;
    case 0x31E: /* лимитер (нет в K-матрице, найден по логу 29.09) */
        s_data.limiter_raw = sig(d, 12, 10);
        touch(S_LIM);
        break;
    case 0x2A9: /* ACC_15: AWV (Audi pre sense front) — ⚠️ раскладка из opendbc MQB,
                   на I-CAN MLB-Evo не проверено */
        s_data.awv_warn = sig(d, 16, 3);
        touch(S_AWV);
        break;
    case 0x5F0: /* Dimmung_01 — DI_KL_58xd 0|8 (%), Nachtdesign 15 — проверено по логу 0003 */
        s_data.dim_raw   = sig(d, 0, 8);
        s_data.dim_pct   = sig(d, 8, 7);
        s_data.dim_night = sig(d, 15, 1);
        touch(S_DIM);
        break;
    case 0x5A0: /* RLS_01 — датчик света и дождя (K-матрица I-CAN, поездки 0006/0010) */
        s_data.rls_ir    = sig(d, 0, 8);
        s_data.rls_fw    = sig(d, 8, 10);
        s_data.rls_rain  = sig(d, 24, 4);
        s_data.rls_boost = sig(d, 35, 4);
        touch(S_RLS);
        break;
    case 0x30F: /* SWA_01 — Side Assist (opendbc MLB/MQB; ⚠️ на I-CAN проверить) */
        s_data.swa_info = (uint8_t)(sig(d, 26, 1) | (sig(d, 42, 1) << 1));   /* Infostufe_SWA_li / _re */
        s_data.swa_warn = (uint8_t)(sig(d, 27, 1) | (sig(d, 43, 1) << 1));   /* Warnung_SWA_li / _re   */
        touch(S_SWA);
        break;
    case 0x64F: /* BCM1_04 — положение колёсика подсветки (лог 0003) */
        { uint8_t w = sig(d, 25, 7); if (w >= 1 && w <= 100) { s_data.wheel_pct = w; touch(S_WHEEL); } }
        break;
    case 0x6B2: /* Diagnose_01: дата и время */
        decode_clock(d);
        break;
    case 0x583: /* ZV_02 */
        decode_zv02(d);
        touch(S_DOORS);
        break;
    case 0x65A: /* BCM_01 */
        decode_bcm01(d);
        touch(S_HOOD);
        break;
    default:
        break;
    }
    taskEXIT_CRITICAL(&s_mux);
}
