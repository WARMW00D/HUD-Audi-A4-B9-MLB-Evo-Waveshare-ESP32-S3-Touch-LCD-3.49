#ifndef HUD_SOUND_H
#define HUD_SOUND_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Запускает звуковую задачу (кодек ES8311 + I2S) и, если включено,
   стартовый звук. Звук играет в своей задаче и от отрисовки не зависит. */
void hud_sound_start(void);

/* Поставить звук в очередь (PCM 16 бит моно HUD_SOUND_RATE). Не блокирует. */
void hud_sound_play(const int16_t *pcm, uint32_t samples);

/* Громкость 0..100 на ходу (под будущее меню / голос) */
void hud_sound_set_volume(uint8_t vol);
uint8_t hud_sound_get_volume(void);

#ifdef __cplusplus
}
#endif
#endif
