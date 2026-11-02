/*
  hud_anim.cpp — чтение /arrows.bin из FATFS в PSRAM. См. hud_anim.h.
*/
#include <Arduino.h>
#include <FFat.h>
#include "esp_heap_caps.h"
#include "hud_log.h"
#include "hud_anim.h"

#define ARROWS_PATH "/arrows.bin"

/* FATFS уже смонтирован вызывающим (hud_cams.cpp) и размонтируется им же */
extern "C" bool hud_anim_load_file(void)
{
    File f = FFat.open(ARROWS_PATH, "r");
    if (!f) return false;                                       /* файла нет — обычные стрелки */
    size_t n = f.size();
    uint8_t *buf = (n >= 128) ? (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!buf) { arduino_printf("[anim] нет памяти под стрелки (%u байт)\n", (unsigned)n); f.close(); return false; }
    size_t got = 0;
    while (got < n) {
        size_t k = f.read(buf + got, n - got > 16384 ? 16384 : n - got);
        if (k == 0) break;
        got += k;
        vTaskDelay(1);                                          /* не мешаем экрану и CAN */
    }
    f.close();
    if (got == n && hud_anim_set_data(buf, n)) {
        arduino_printf("[anim] стрелки загружены: %u КБ\n", (unsigned)(n / 1024));
        return true;
    }
    hud_log_write("[anim] arrows.bin повреждён или другой версии — обычные стрелки\n");
    heap_caps_free(buf);
    return false;
}
