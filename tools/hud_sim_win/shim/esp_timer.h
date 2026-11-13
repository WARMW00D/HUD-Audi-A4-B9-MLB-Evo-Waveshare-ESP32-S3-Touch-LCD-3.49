/* Заглушка esp_timer.h: время HUD = время проигрываемого лога (см. sim_core.c) */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int64_t esp_timer_get_time(void);   /* мкс */
#ifdef __cplusplus
}
#endif
