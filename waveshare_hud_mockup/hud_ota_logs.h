/*
  hud_ota_logs.h — журналы с SD-карты на странице режима обновления (как на портале сниффера):
  /logs — список папок по датам, /download — один файл, /download-tar — выбранные папки одним TAR,
  /delete — удаление выбранных папок. Подключается к WebServer из hud_ota.cpp.
*/
#pragma once
#include <WebServer.h>

/* touch — вызывать при каждом запросе и во время долгой отдачи файла: сбрасывает таймаут режима обновления */
void hud_ota_logs_attach(WebServer &srv, void (*touch)(void));
