/*
  hud_cams.cpp — см. hud_cams.h
*/
#include <Arduino.h>
#include <FFat.h>
#include "esp_heap_caps.h"
#include "hud_config.h"
#include "hud_log.h"
#include "hud_cams.h"
#include "hud_diag.h"
extern "C" {
#include "cam_alert.h"
}

#define CAMS_PATH "/cams.bin"

static volatile bool s_ready = false;

static void load_task(void *)
{
#if HUD_CAMS
    if (!FFat.begin(false)) {                                   /* нет раздела FATFS или он не размечен */
        hud_log_write("[cam] FATFS не смонтирован — базы камер нет\n");
        g_diag.db_status = DB_NO_FATFS;
        vTaskDelete(NULL);
        return;
    }
    File f = FFat.open(CAMS_PATH, "r");
    if (!f) { g_diag.db_status = DB_NO_FILE; hud_log_write("[cam] /cams.bin не найден\n"); FFat.end(); vTaskDelete(NULL); return; }
    size_t n = f.size();
    uint8_t *buf = (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { g_diag.db_status = DB_NO_MEM; arduino_printf("[cam] нет памяти под базу (%u байт)\n", (unsigned)n); f.close(); FFat.end(); vTaskDelete(NULL); return; }
    size_t got = 0;
    while (got < n) {
        size_t k = f.read(buf + got, n - got > 4096 ? 4096 : n - got);
        if (k == 0) break;
        got += k;
        vTaskDelay(1);                                          /* не мешаем экрану и CAN */
    }
    f.close();
    FFat.end();
    cam_cfg_t c = { (float)HUD_CAM_AHEAD_S, (float)HUD_CAM_MIN_M, (float)HUD_CAM_MAX_M,
                    (float)HUD_CAM_CROSS_M, (float)HUD_CAM_DIR_DEG, (uint16_t)HUD_CAM_TYPE_MASK };
    cam_config(&c);
    if (got == n && cam_load(buf, n)) {
        s_ready = true;
        g_diag.db_status = DB_OK; g_diag.db_n = cam_count(); g_diag.db_date = cam_date();
        arduino_printf("[cam] база загружена: %u камер, от %06u\n", (unsigned)cam_count(), (unsigned)cam_date());
    } else {
        g_diag.db_status = DB_BAD;
        hud_log_write("[cam] cams.bin повреждён или другой версии\n");
        heap_caps_free(buf);
    }
#endif
    vTaskDelete(NULL);
}

extern "C" void hud_cams_start(void)
{
#if HUD_CAMS
    xTaskCreatePinnedToCore(load_task, "hud_cams", 6144, NULL, 1, NULL, 0);
#endif
}

extern "C" bool hud_cams_ready(void) { return s_ready; }
