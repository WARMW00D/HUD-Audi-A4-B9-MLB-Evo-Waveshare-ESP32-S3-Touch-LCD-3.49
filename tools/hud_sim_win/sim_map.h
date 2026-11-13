/* Окно карты: локальный HTTP-сервер (127.0.0.1) отдаёт map.html и текущие координаты из лога (NavPos_01, 0x486). */
#ifndef SIM_MAP_H
#define SIM_MAP_H
#ifdef __cplusplus
extern "C" {
#endif
/* html_path — полный путь к map.html; yandex_key / gis_key — необязательные ключи по умолчанию (UTF-8) */
int  map_server_start(int port, const wchar_t *html_path, const char *yandex_key, const char *gis_key);
int  map_server_port(void);                       /* 0 — сервер не запущен */
/* вызывать из основного потока при каждом такте; valid = есть координаты с фиксом */
void map_update(int loaded, int valid, double lat, double lon, int hdg_x10, int speed, int sat, int fix, unsigned t_ms);
#ifdef __cplusplus
}
#endif
#endif
