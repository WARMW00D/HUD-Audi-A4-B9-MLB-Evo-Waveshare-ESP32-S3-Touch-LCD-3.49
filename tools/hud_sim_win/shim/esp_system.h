/* Заглушка esp_system.h для сборки HUD под ПК (hud_sim_win) */
#pragma once
#include <stdint.h>
static inline uint32_t esp_get_free_heap_size(void) { return 0; }
static inline uint32_t esp_get_minimum_free_heap_size(void) { return 0; }
static inline void esp_restart(void) {}
