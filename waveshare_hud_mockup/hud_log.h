#ifndef HUD_LOG_H
#define HUD_LOG_H
#ifdef __cplusplus
extern "C" {
#endif
/* Единая точка вывода журнала: Serial и (HUD_LOG_TO_SD) файл на SD-карте.
   Вызывать можно из любой задачи; запись на карту — в своей задаче, через буфер. */
void hud_log_init(void);
void hud_log_write(const char *s);
void arduino_printf(const char *format, ...);   /* printf -> hud_log_write */
/* Смонтировать SD-карту платы (SPI: CS 38, MOSI 39, MISO 40, SCLK 41), если ещё не смонтирована.
   Общая для журнала и страницы журналов в режиме обновления; можно вызывать из любой задачи. */
bool hud_sd_ready(void);
#ifdef __cplusplus
}
#endif
#endif
