/* lv_conf.h для Waveshare ESP32-S3-Touch-LCD-3.49 (LVGL 8.3/8.4).
   Скопировать в Documents/Arduino/libraries/lv_conf.h — РЯДОМ с папкой lvgl, не внутрь.
   Не совпадает с lv_conf.h проекта supermini_hud (там LV_COLOR_16_SWAP 0): при переключении между
   проектами подменяйте файл. Всё, чего здесь нет, берётся по умолчанию из lv_conf_internal.h. */
#if 1                                  /* "1" — включить содержимое */
#ifndef LV_CONF_H
#define LV_CONF_H
#include <stdint.h>

#define LV_COLOR_DEPTH          16
#define LV_COLOR_16_SWAP        1      /* порядок байт панели AXS15231B */
#define LV_COLOR_SCREEN_TRANSP  1      /* нужно для прозрачности (opa) и перекраски */

/* Память LVGL: 48 КБ мало — при нехватке LVGL вешал экран (while(1)) до перезагрузки сторожем.
   Пул кладём в PSRAM, внутреннюю память не занимаем. */
#define LV_MEM_CUSTOM           0
#define LV_MEM_SIZE             (96U * 1024U)
#define LV_MEM_ADR              0
#if LV_MEM_ADR == 0
    #define LV_MEM_POOL_INCLUDE     "esp_heap_caps.h"
    #define LV_MEM_POOL_ALLOC(size) heap_caps_malloc(size, MALLOC_CAP_SPIRAM)
#endif

#define LV_IMG_CACHE_DEF_SIZE   1

#define LV_USE_FONT_COMPRESSED  1      /* ОБЯЗАТЕЛЬНО: иначе шрифты HUD не рисуются */
#define LV_USE_LOG              1
#define LV_LOG_LEVEL            LV_LOG_LEVEL_WARN

/* Нехватка памяти или нулевой указатель: быстрая перезагрузка вместо зависания на while(1) */
#define LV_ASSERT_HANDLER_INCLUDE "esp_system.h"
#define LV_ASSERT_HANDLER       esp_restart();

#endif /*LV_CONF_H*/
#endif /*End of "Content enable"*/
