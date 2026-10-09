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
#ifdef __cplusplus
}
#endif
#endif
