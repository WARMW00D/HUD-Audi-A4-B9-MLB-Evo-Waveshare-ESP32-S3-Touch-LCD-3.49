/*
  vze_table.h — расшифровка знаков для HUD
  ------------------------------------------------------------------------------
  1) vze_table[] — код VZE_01.VZE_Verkehrszeichen_1 -> тип и значение: исключения
     из формулы 5 * код (по умолчанию пусто — действует формула, см. ниже).
     Формат строки:  [0x5A] = { VZE_LIMIT, 60 },
  2) acc_tempolimit_kmh() — ACC_12.ACC_Tempolimit (0x2A6, 0|5), таблица из K-матрицы.
     Используется, когда кода нет в vze_table.

  Порядок показа на HUD:
    код в таблице           -> знак из таблицы
    код 1..30               -> 5 * код км/ч (формула, см. vze_linear_kmh)
    кода нет, Tempolimit 1..28/30 -> ограничение из Tempolimit
    кода нет, Tempolimit 31 -> "конец ограничения"
    ничего нет              -> сырой код мелким серым (чтобы дописать таблицу)
*/
#ifndef VZE_TABLE_H
#define VZE_TABLE_H
#include <stdint.h>

typedef enum {
    VZE_UNKNOWN = 0,   /* кода нет в таблице              */
    VZE_LIMIT,         /* ограничение, value = км/ч (mph) */
    VZE_END_LIMIT,     /* конец ограничения               */
    VZE_NO_OVERTAKE,   /* запрет обгона (пока не рисуется) */
    VZE_END_OVERTAKE,  /* конец запрета обгона (пока не рисуется) */
} VzeType;

typedef struct { uint8_t type; uint8_t value; } VzeEntry;

static const VzeEntry vze_table[256] = {
    /* [0x00] — нет знака */
    /* [0x??] = { VZE_LIMIT, 60 }, */
    [0] = { VZE_UNKNOWN, 0 },
};

/* Линейная формула кода VZE: км/ч = 5 * код. Проверено в поездке:
   код 8 = 40, код 12 = 60, код 16 = 80 км/ч. Применяется к кодам
   VZE_LINEAR_MIN..VZE_LINEAR_MAX, если кода нет в vze_table[].
   Если какой-то код окажется другим знаком (обгон, конец ограничения) —
   впишите его в vze_table[], таблица главнее формулы. */
#define VZE_LINEAR_MIN  1     /* 5 км/ч   */
#define VZE_LINEAR_MAX  30    /* 150 км/ч */

static inline uint16_t vze_linear_kmh(uint8_t code)
{
    return (code >= VZE_LINEAR_MIN && code <= VZE_LINEAR_MAX) ? (uint16_t)(5 * code) : 0;
}

/* ACC_Tempolimit: 0 нет, 31 конец ограничения, 29 не определено */
static const uint8_t acc_tempolimit_tbl[32] = {
      0,   5,   7,  10,  15,  20,  25,  30,  35,  40,
     45,  50,  55,  60,  65,  70,  75,  80,  85,  90,
     95, 100, 110, 120, 130, 140, 150, 160, 200,   0,
    250,   0
};
#define ACC_TEMPOLIMIT_END  31

static inline uint16_t acc_tempolimit_kmh(uint8_t raw)
{
    return raw < 32 ? acc_tempolimit_tbl[raw] : 0;
}

#endif
