/* Заглушка FreeRTOS для сборки HUD под ПК: симулятор однопоточный, блокировки не нужны */
#pragma once
#include <stdint.h>
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(m)  ((void)(m))
#define taskEXIT_CRITICAL(m)   ((void)(m))
#define portENTER_CRITICAL(m)  ((void)(m))
#define portEXIT_CRITICAL(m)   ((void)(m))
