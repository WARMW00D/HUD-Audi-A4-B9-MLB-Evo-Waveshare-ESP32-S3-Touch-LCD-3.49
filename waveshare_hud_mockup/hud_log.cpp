/*
  hud_log.cpp — журнал HUD: Serial + (по желанию) файл на SD-карте платы
  ------------------------------------------------------------------------------
  Все модули печатают через arduino_printf() / hud_log_write(). Строки сразу
  уходят в Serial, а при HUD_LOG_TO_SD — ещё и в потоковый буфер, из которого
  отдельная задача (ядро 1, низкий приоритет) пишет их на карту.

  Файлы создаются только после того, как по CAN пришли дата и время машины
  (Diagnose_01): /logs/ГГГГ-ММ-ДД/log_ЧЧ_N.txt, ЧЧ — час открытия файла,
  N — 0, 1, 2… если за этот час файл уже есть (перезапуск). Каждый новый час —
  новый файл. Строки до появления часов копятся в буфере и попадают в начало
  первого файла с меткой [+мс от включения], дальше — [ЧЧ:ММ:СС.мс].

  SD-карта — SPI: CS 38, MOSI 39, MISO 40, SCLK 41 (таблица GPIO Waveshare 3.49).
  Хост SPI2 (FSPI): дисплей занимает SPI3, конфликта нет.
  Карты нет или не смонтировалась — пишем только в Serial, ошибок нет.
*/
#include <Arduino.h>
#include <stdarg.h>
#include "hud_config.h"
#include "hud_log.h"

#if HUD_LOG_TO_SD
#include <SPI.h>
#include <SD.h>
#include "freertos/stream_buffer.h"
#include "hud_data.h"

#define SD_CS    38
#define SD_MOSI  39
#define SD_MISO  40
#define SD_SCK   41

static StreamBufferHandle_t s_sb;
static volatile uint32_t    s_dropped;
static volatile bool        s_line_start = true;
static SPIClass             s_spi(FSPI);

/* /logs/ГГГГ-ММ-ДД/log_ЧЧ_N.txt — первый свободный N для этого часа */
static bool sd_open_hour(File &f, const HudClock &c)
{
    char dir[32], name[48];
    if (!SD.exists("/logs")) SD.mkdir("/logs");
    snprintf(dir, sizeof dir, "/logs/%04u-%02u-%02u", c.year, c.mon, c.day);
    if (!SD.exists(dir)) SD.mkdir(dir);
    for (unsigned n = 0; n < 1000; n++) {
        snprintf(name, sizeof name, "%s/log_%02u_%u.txt", dir, c.hour, n);
        if (!SD.exists(name)) {
            f = SD.open(name, FILE_WRITE);
            if (f) Serial.printf("[log] журнал на SD: %s\n", name);
            return (bool)f;
        }
    }
    return false;
}

static void sd_stop(const char *why)
{
    Serial.println(why);
    StreamBufferHandle_t sb = s_sb;
    s_sb = NULL;                        /* sd_push больше не пишет в буфер */
    vTaskDelay(pdMS_TO_TICKS(50));
    if (sb) vStreamBufferDelete(sb);
    vTaskDelete(NULL);
}

static void sd_task(void *)
{
    s_spi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, s_spi, 20000000)) { sd_stop("[log] SD-карта не найдена — журнал только в Serial"); return; }

    /* ждём дату и время по CAN; всё, что напечатано до этого, копится в буфере
       (16 КБ) и попадёт в начало файла */
    HudClock c;
    while (!hud_clock_now(&c)) vTaskDelay(pdMS_TO_TICKS(200));

    File f;
    if (!sd_open_hour(f, c)) { sd_stop("[log] не удалось создать файл журнала на SD"); return; }
    uint8_t cur_hour = c.hour, cur_day = c.day;

    static uint8_t buf[1024];
    uint32_t last_flush = millis();
    for (;;) {
        size_t n = xStreamBufferReceive(s_sb, buf, sizeof buf, pdMS_TO_TICKS(500));
        if (n) f.write(buf, n);
        uint32_t now = millis();
        if (now - last_flush < 2000) continue;           /* сброс на карту раз в 2 с */
        last_flush = now;
        if (s_dropped) {
            f.printf("[log] пропущено %lu байт (буфер полон)\n", (unsigned long)s_dropped);
            s_dropped = 0;
        }
        f.flush();
        /* новый файл: сменился час (или день), или файл слишком большой */
        hud_clock_now(&c);
        bool big = f.size() > (uint32_t)HUD_LOG_SD_MAX_MB * 1024u * 1024u;
        if (c.hour != cur_hour || c.day != cur_day || big) {
            f.close();
            if (!sd_open_hour(f, c)) { sd_stop("[log] не удалось создать файл журнала на SD"); return; }
            cur_hour = c.hour; cur_day = c.day;
        }
    }
}

/* Метка времени в начале строки: [ЧЧ:ММ:СС.мс] по часам машины,
   а пока часов нет — [+мс от включения]. */
static void sd_push(const char *s)
{
    StreamBufferHandle_t sb = s_sb;
    if (!sb) return;
    while (*s) {
        if (s_line_start) {
            char ts[24];
            HudClock c;
            int k = hud_clock_now(&c)
                  ? snprintf(ts, sizeof ts, "[%02u:%02u:%02u.%03u] ", c.hour, c.min, c.sec, c.ms)
                  : snprintf(ts, sizeof ts, "[+%lu] ", (unsigned long)millis());
            if (xStreamBufferSend(sb, ts, k, 0) != (size_t)k) s_dropped += k;
            s_line_start = false;
        }
        const char *nl = strchr(s, '\n');
        size_t len = nl ? (size_t)(nl - s + 1) : strlen(s);
        if (xStreamBufferSend(sb, s, len, 0) != len) s_dropped += len;
        if (nl) s_line_start = true;
        s += len;
    }
}
#endif

void hud_log_init(void)
{
#if HUD_LOG_TO_SD
    s_sb = xStreamBufferCreate(16 * 1024, 1);
    if (s_sb) xTaskCreatePinnedToCore(sd_task, "hud_sdlog", 4096, nullptr, 1, nullptr, 1);
#endif
}

void hud_log_write(const char *s)
{
    Serial.print(s);
#if HUD_LOG_TO_SD
    sd_push(s);
#endif
}

extern "C" void arduino_printf(const char *format, ...)
{
    char loc_buf[256];
    va_list arg;
    va_start(arg, format);
    vsnprintf(loc_buf, sizeof(loc_buf), format, arg);
    va_end(arg);
    hud_log_write(loc_buf);
}
