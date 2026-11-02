/*
  hud_anim.h — анимированные стрелки навигации из файла /arrows.bin (раздел FATFS)
  ------------------------------------------------------------------------------
  Файл делает tools/gen_arrow_anim.py и заливается на странице обновления (рядом с cams.bin).
  Целиком читается в PSRAM при старте; кадр — независимый блок LZ4, распаковывается в буфер
  4-битной картинки с палитрой (LV_IMG_CF_INDEXED_4BIT) за ~1 мс. Нет файла — hud_anim_ready()
  возвращает false, и HUD рисует обычные стрелки.
*/
#ifndef HUD_ANIM_H
#define HUD_ANIM_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

bool hud_anim_ready(void);
/* разобрать файл, уже лежащий в памяти (буфер должен жить всегда). false — формат неверный */
bool hud_anim_set_data(const uint8_t *buf, size_t len);
/* прочитать /arrows.bin в PSRAM; FATFS уже должен быть смонтирован (вызывается из загрузчика базы камер) */
bool hud_anim_load_file(void);

int  hud_anim_find(const char *name);                 /* номер анимации или -1 */
int  hud_anim_frames(int anim);                       /* число кадров */
/* распаковать кадр; NULL при ошибке. Возвращённая картинка действует до двух следующих вызовов */
const lv_img_dsc_t *hud_anim_frame(int anim, int frame);

#ifdef __cplusplus
}
#endif
#endif
