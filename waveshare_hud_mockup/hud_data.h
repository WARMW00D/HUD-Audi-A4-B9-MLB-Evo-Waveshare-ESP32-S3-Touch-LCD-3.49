/*
  hud_data.h — общее состояние HUD между декодером CAN (BLE-задача)
  и отрисовкой (lv_timer внутри задачи LVGL).

  Пишет только декодер (can_decode.c), читает только hud_update через
  hud_data_snapshot(): копия под portMUX, биты valid считаются по
  таймаутам в момент снимка.
*/
#ifndef HUD_DATA_H
#define HUD_DATA_H
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Биты HudData.valid */
#define V_SPEED  (1u << 0)   /* speed_kmh                         */
#define V_ACC    (1u << 1)   /* acc_set_raw, acc_status           */
#define V_GEAR   (1u << 2)   /* gear                              */
#define V_BLINK  (1u << 3)   /* blink                             */
#define V_SIGN   (1u << 4)   /* sign_raw, sign_mph                */
#define V_MAN    (1u << 5)   /* rg_active, man_main, man_dir      */
#define V_MDIST  (1u << 6)   /* man_dist_x10, man_unit, bargraph  */
#define V_DEST   (1u << 7)   /* dest_dist_x10, dest_unit          */
#define V_TTE    (1u << 8)   /* tte_type, tte_h, tte_min          */
#define V_LKA    (1u << 9)   /* lka_state, lka_line_*, lka_warn   */
#define V_DOORS  (1u << 10)  /* doors: FL FR RL RR TRUNK (ZV_02)  */
#define V_HOOD   (1u << 11)  /* doors: HOOD (BCM_01)              */
#define V_GNUM   (1u << 12)  /* gear_num                          */
#define V_JAM    (1u << 13)  /* jam_state (из ACC_12)             */
#define V_LIM    (1u << 14)  /* limiter_raw                       */
#define V_AWV    (1u << 15)  /* awv_warn (pre sense)              */
#define V_DIM    (1u << 16)  /* dim_*  (Dimmung_01)               */
#define V_ACCEL  (1u << 17)  /* accel_esp_x100 (ESP_02)           */
#define V_SPDACC (1u << 18)  /* accel_spd_x100 (по скорости)      */
#define V_BRAKE  (1u << 19)  /* brake_bar, brake_pedal (ESP_05)   */
#define V_FUEL   (1u << 20)  /* fuel_l100_x10 (Motor_04)          */
#define V_CLOCK  (1u << 21)  /* часы машины свежие (Diagnose_01)  */
#define V_BC     (1u << 22)  /* бортовой компьютер (BAP_BC 0x0F)  */
#define V_BCFUEL (1u << 23)  /* bc_fuel_pct (BAP_BC fct 0x1C)     */
#define V_RLS    (1u << 24)  /* rls_* (RLS_01 0x5A0)              */
#define V_WHEEL  (1u << 25)  /* wheel_pct (BCM1_04 0x64F)         */
#define V_SWA    (1u << 26)  /* swa_* (SWA_01 0x30F)              */

#define BLINK_L_TAKT  0x01
#define BLINK_R_TAKT  0x02
#define BLINK_HAZARD  0x04

/* HudData.doors */
#define DOOR_FL    0x01
#define DOOR_FR    0x02
#define DOOR_RL    0x04
#define DOOR_RR    0x08
#define DOOR_HOOD  0x10
#define DOOR_TRUNK 0x20

/* LKA_PASSIVE = жёлтый (включён, но не держит), LKA_ACTIVE = зелёный,
   LKA_INTERVENE = предупреждение о выходе за линию */
enum { LKA_OFF = 0, LKA_PASSIVE = 1, LKA_ACTIVE = 2, LKA_INTERVENE = 3 };

typedef struct {
    uint16_t speed_kmh;      /* Kombi_01.KBI_V_Digital                  */
    uint16_t acc_set_raw;    /* ACC_12.ACC_Wunschgeschw_02, ×0.32 км/ч  */
    uint8_t  acc_status;     /* ACC_14.ACC_Status_Anzeige               */
    uint8_t  gear;           /* WBA_03.WBA_Fahrstufe_02                 */
    uint8_t  gear_num;       /* номер передачи 1..9, 0 = нет (WBA_03.WBA_eing_Gang_02) */
    uint8_t  blink;          /* BLINK_*                                 */
    uint8_t  sign_raw;       /* VZE_01.VZE_Verkehrszeichen_1            */
    uint8_t  sign_mph;       /* VZE_01.VZE_Verkehrszeichen_Einheit      */
    uint8_t  sign_suppress;  /* VZE_Anzeigeunterdrueck_Zeichen_1        */
    uint8_t  sign_raw2, sign_raw3;           /* VZE_Verkehrszeichen_2 / _3 (19|8, 27|8) */
    uint8_t  sign_sup2, sign_sup3;           /* Anzeigeunterdrueck_Zeichen_2 / _3 (10, 9) */
    uint8_t  sign_warn;      /* VZE_Warnung_Verkehrszeichen_1 (превышение) */
    uint8_t  acc_tempolimit; /* ACC_12.ACC_Tempolimit (сырое 0..31)     */
    uint8_t  lka_state;      /* LKA_*                                   */
    uint8_t  lka_line_l;     /* LDW_Lernmodus_links: 0 выкл 1 нет 2 видна 3 выход */
    uint8_t  lka_line_r;     /* LDW_Lernmodus_rechts                    */
    uint8_t  lka_warn;       /* бит0 слева, бит1 справа                 */
    uint8_t  doors;          /* DOOR_*: 1 = открыто                     */
    uint8_t  jam_state;      /* ACC_12.STA_Primaeranz: 0 нет, 1 готов, 2 активен, 3 предупр. */
    uint16_t limiter_raw;    /* 0x31E 12|10: ×0.32 км/ч, 1022 выкл, 1023 вкл без скорости */
    uint8_t  acc_object;     /* ACC_12.ACC_Relevantes_Objekt_02: 0 нет, 1 машина впереди, 2 предупр., 3 пассив */
    uint8_t  acc_gap;        /* ACC_12.ACC_Gesetzte_Zeitluecke: дистанция 1..5, 0 нет */
    uint8_t  aca_lane;       /* ACC_12.ACA_Querfuehrung: 0 нет, 1 пассив, 2 активно, 3 предупр. */
    uint8_t  awv_warn;       /* ACC_15.AWV_Warnung: 0 нет, 1 латентная, 2 предупр., 3 острая,
                                4 торможение, 5 перехватите управление, 6 при повороте */
    uint8_t  dim_raw;        /* Dimmung_01.DI_KL_58xd: авто-яркость дисплеев 10..253 (254 init, 255 ошибка) */
    uint8_t  dim_pct;        /* Dimmung_01.DI_KL_58xs: яркость дисплеев 0..100 % */
    uint8_t  dim_night;      /* Dimmung_01.DI_Display_Nachtdesign        */
    uint16_t rls_fw;         /* RLS_01.LS_Helligkeit_FW, x6 лк (0..1021; 1022 init, 1023 ошибка) */
    uint8_t  rls_ir;         /* RLS_01.LS_Helligkeit_IR, x400 лк         */
    uint8_t  rls_boost;      /* RLS_01.RLS_Vorfeldhelligkeit_Boost 0..15 (0 — очень яркое солнце) */
    uint8_t  rls_rain;       /* RLS_01.RS_Regenmenge, x10 %              */
    uint8_t  swa_info;       /* SWA_01: Infostufe — машина в мёртвой зоне: бит0 слева, бит1 справа */
    uint8_t  swa_warn;       /* SWA_01: Warnung — попытка перестроения при машине сбоку: бит0/бит1 */
    uint8_t  wheel_pct;      /* BCM1_04.BCM1_Stellgroesse_Kl_58s — колёсико подсветки 1..100 */
    int16_t  accel_esp_x100; /* ESP_02.ESP_Laengsbeschl, м/с² x100 (+ разгон) */
    int16_t  accel_spd_x100; /* производная скорости Kombi_01, м/с² x100, сглаженная */
    int16_t  brake_bar;      /* ESP_05.ESP_Bremsdruck, бар              */
    uint8_t  brake_pedal;    /* ESP_05.ESP_Fahrer_bremst               */
    uint16_t fuel_l100_x10;  /* мгновенный расход, л/100 км x10 (Motor_04.MO_KVS) */
    uint16_t fuel_avg_x10;   /* средний расход с момента запуска HUD, л/100 км x10; 0xFFFF — мало данных */

    /* бортовой компьютер приборки, BAP_BC (LSG 0x0F, 0x17330F10) */
    uint16_t bc_avg_x10;     /* fct 0x18 «с момента запуска»: средний расход, л/100 км x10; 0xFFFF — нет */
    uint16_t bc_dist_x10;    /*   пробег, км x10                 */
    uint8_t  bc_time_min;    /*   время в пути, мин              */
    uint16_t bc_vavg_x10;    /*   средняя скорость, км/ч x10     */
    uint16_t bcl_avg_x10;    /* fct 0x19 «долговременная память»: те же поля */
    uint16_t bcl_dist_x10;
    uint8_t  bcl_time_min;
    uint16_t bcl_vavg_x10;
    uint16_t bc_range_km;    /* fct 0x16 запас хода, км         */
    uint32_t bc_odo_x10;     /* fct 0x17 одометр, км x10        */
    uint8_t  bc_fuel_pct;    /* fct 0x1C уровень топлива, %     */

    uint8_t  rg_active;      /* BAP 0x11: 0 нет, 1 ведётся, 2 пауза     */
    uint8_t  man_main;       /* BAP 0x17 MainElement                    */
    uint8_t  man_dir;        /* BAP 0x17 Direction (360/256 град)       */
    uint8_t  man_z;          /* BAP 0x17 Z-level                        */
    uint8_t  man2_main;      /* BAP 0x17 Maneuver_2 (следующий), 0 = нет */
    uint8_t  man2_dir;
    uint8_t  man_side_n;     /* BAP 0x17 число боковых улиц (Sidestreets) */
    uint8_t  man_side[8];    /* направления боковых улиц (первые 8)     */
    uint8_t  man_raw_len;    /* длина сообщения 0x17 (диагностика)      */
    uint8_t  man_raw[32];    /* сообщение 0x17 целиком, первые 32 байта (диагностика) */
    uint32_t man_dist_x10;   /* BAP 0x12                                */
    uint8_t  man_unit;       /* 0 м, 1 км, 2 ярд, 3 фут, 4 миля, 5 1/4мили */
    uint8_t  bargraph;       /* 0..100, 0xFF = не показывать            */
    uint32_t dest_dist_x10;  /* BAP 0x15                                */
    uint8_t  dest_unit;
    uint8_t  tte_type;       /* 0 время в пути, 1 время прибытия        */
    uint8_t  tte_h, tte_min; /* BAP 0x16                                */

    uint32_t valid;          /* V_*                                     */
    bool     link_up;        /* BLE-связь со сниффером                  */
    uint32_t frames_rx;      /* статистика                              */
    uint32_t batches_lost;   /* пачек с флагом потерь                   */
} HudData;

uint32_t hud_now_ms(void);
void hud_data_snapshot(HudData *out);
void hud_data_set_link(bool up);

/* Дата и время машины (Diagnose_01, 0x6B2), продолженные по millis() между кадрами.
   false — часы ещё ни разу не приходили или недостоверны. */
typedef struct { uint16_t year; uint8_t mon, day, hour, min, sec; uint16_t ms; } HudClock;
bool hud_clock_now(HudClock *c);
void hud_data_note_lost(void);
uint32_t hud_data_take_speed_gap_max(void);
void hud_data_debug_print(void (*out)(const char *line));
void hud_data_fake_doors(uint8_t mask);   /* только для HUD_SRC_FAKE */

/* Вход декодера: один CAN-кадр (RTR сюда не подавать) */
void can_decode_frame(uint32_t id, bool ext, const uint8_t *data, uint8_t dlc);

#ifdef __cplusplus
}
#endif
#endif
