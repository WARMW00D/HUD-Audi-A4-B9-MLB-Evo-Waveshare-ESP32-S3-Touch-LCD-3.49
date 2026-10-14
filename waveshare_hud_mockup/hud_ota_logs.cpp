/*
  hud_ota_logs.cpp — чтение и удаление журналов с SD-карты через веб-страницу режима обновления.
  ------------------------------------------------------------------------------
  Журналы лежат в /logs/ГГГГ-ММ-ДД/log_ЧЧ_N.txt (hud_log.cpp, HUD_LOG_TO_SD).
  Страница /logs: папки по датам (свёрнуты, по «+» — файлы с размерами), галочка у папки,
  кнопки «Скачать выбранное одним TAR» и «Удалить выбранные». Отдельный файл — ссылкой.
  Скачивание отдаёт ровно объявленный размер, куски дописываются с повторами (как в сниффере);
  TAR собирается «на лету» без сжатия. В режиме обновления CAN не работает, часов машины нет,
  поэтому файл на запись не открыт и удалять можно любые папки.
  Работают только пути вида /logs/ГГГГ-ММ-ДД и /logs/ГГГГ-ММ-ДД/имя — всё остальное отвергается.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SD.h>
#include <vector>
#include <algorithm>
#include "hud_log.h"
#include "hud_ota_logs.h"

static WebServer *s_srv;
static void     (*s_keepalive)(void);
static inline void keepalive() { if (s_keepalive) s_keepalive(); }

/* ---------------- проверка путей ---------------- */
/* "/logs/ГГГГ-ММ-ДД" */
static bool is_date_dir(const String &p)
{
    if (p.length() != 16 || !p.startsWith("/logs/")) return false;
    for (int i = 0; i < 10; i++) {
        char c = p[6 + i];
        if (i == 4 || i == 7) { if (c != '-') return false; }
        else if (c < '0' || c > '9') return false;
    }
    return true;
}

/* "/logs/ГГГГ-ММ-ДД/имя": имя из латиницы, цифр и _ . -, не скрытое */
static bool is_log_name(const String &n)
{
    if (n.length() == 0 || n.length() > 40 || n[0] == '.') return false;
    for (unsigned i = 0; i < n.length(); i++) {
        char c = n[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.' || c == '-')) return false;
    }
    return true;
}

static bool is_log_file(const String &p)
{
    return p.length() > 17 && is_date_dir(p.substring(0, 16)) && p[16] == '/' && is_log_name(p.substring(17));
}

static String base_name(const char *n)
{
    String s(n);
    int k = s.lastIndexOf('/');
    return k >= 0 ? s.substring(k + 1) : s;
}

static String human_size(uint64_t b)
{
    char buf[24];
    if (b < 1024)               snprintf(buf, sizeof buf, "%u B", (unsigned)b);
    else if (b < 1024ULL << 20) snprintf(buf, sizeof buf, b < 1024ULL * 1024 ? "%.1f KB" : "%.1f MB",
                                         b < 1024ULL * 1024 ? b / 1024.0 : b / 1048576.0);
    else                        snprintf(buf, sizeof buf, "%.2f GB", b / 1073741824.0);
    return String(buf);
}

/* ---------------- списки ---------------- */
struct FileItem { String name; uint32_t size; uint32_t mtime; };

static std::vector<String> list_dates()                       /* новые сверху */
{
    std::vector<String> v;
    File root = SD.open("/logs");
    if (!root || !root.isDirectory()) return v;
    for (File e = root.openNextFile(); e; e = root.openNextFile()) {
        String p = "/logs/" + base_name(e.name());
        if (e.isDirectory() && is_date_dir(p)) v.push_back(p);
        e.close();
    }
    root.close();
    std::sort(v.begin(), v.end(), [](const String &a, const String &b) { return strcmp(a.c_str(), b.c_str()) > 0; });
    return v;
}

static std::vector<FileItem> list_files(const String &dir)    /* по имени, как в папке */
{
    std::vector<FileItem> v;
    File d = SD.open(dir);
    if (!d || !d.isDirectory()) return v;
    for (File e = d.openNextFile(); e; e = d.openNextFile()) {
        String n = base_name(e.name());
        if (!e.isDirectory() && is_log_name(n)) v.push_back({ n, (uint32_t)e.size(), (uint32_t)e.getLastWrite() });
        e.close();
    }
    d.close();
    std::sort(v.begin(), v.end(), [](const FileItem &a, const FileItem &b) { return strcmp(a.name.c_str(), b.name.c_str()) < 0; });
    return v;
}

/* ---------------- страница /logs ---------------- */
static const char LOGS_HEAD[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>HUD logs</title>
<style>body{font-family:sans-serif;max-width:560px;margin:24px auto;padding:0 16px;background:#111;color:#eee}
h1{font-size:20px}a{color:#6cf}button{font-size:16px;padding:10px 18px;margin:8px 8px 0 0;background:#222;color:#eee;border:1px solid #666;border-radius:6px}
button.del{border-color:#c62828;color:#ff6b6b}.meta{color:#999;font-size:13px}
.fold{display:flex;gap:10px;align-items:flex-start;margin:8px 0}.fold input{margin-top:3px;width:18px;height:18px}
summary{cursor:pointer}ul{margin:6px 0 6px 0;padding-left:20px}li{margin:3px 0}</style></head><body>
<h1>HUD — журналы / logs</h1>
<p><a href="/">&larr; на главную / home</a></p>
)HTML";

static void send_chunk(const String &s) { s_srv->sendContent(s); }

static void h_logs()
{
    keepalive();
    s_srv->setContentLength(CONTENT_LENGTH_UNKNOWN);
    s_srv->send(200, "text/html; charset=utf-8", "");
    send_chunk(String(FPSTR(LOGS_HEAD)));
    if (!hud_sd_ready()) {
        send_chunk("<p>SD-карта не найдена / SD card not found</p></body></html>");
        s_srv->sendContent("");
        return;
    }
    std::vector<String> dates = list_dates();
    if (dates.empty()) {
        send_chunk("<p>Папок с журналами пока нет / No log folders yet.<br>"
                   "<span class='meta'>Журнал включается в hud_config.h: HUD_LOG_TO_SD 1 / enable in hud_config.h: HUD_LOG_TO_SD 1</span></p></body></html>");
        s_srv->sendContent("");
        return;
    }
    send_chunk("<form id='f' action='/download-tar' method='GET'>"
               "<p><span class='meta'>Отметьте папки, чтобы скачать их одним TAR-архивом или удалить. «+» — файлы папки. / "
               "Tick folders to download them as one TAR or delete them. Expand a folder to see its files.</span></p>"
               "<p><label><input type='checkbox' id='all' onclick=\"var c=document.querySelectorAll('input[name=folder]');"
               "for(var i=0;i<c.length;i++)c[i].checked=this.checked\"> Отметить все / Select all</label></p>");
    for (const String &dir : dates) {
        std::vector<FileItem> files = list_files(dir);
        uint64_t total = 0;
        String items;
        for (const FileItem &f : files) {
            total += f.size;
            items += "<li><a href='/download?file=" + dir + "/" + f.name + "'>" + f.name + "</a> <span class='meta'>(" + human_size(f.size) + ")</span></li>";
        }
        send_chunk("<div class='fold'><input type='checkbox' name='folder' value='" + dir + "'><details><summary>" + dir.substring(6) +
                   " <span class='meta'>— " + String((unsigned)files.size()) + " файл(ов) / files, " + human_size(total) + "</span></summary><ul>" +
                   (files.empty() ? String("<li><i>пусто / empty</i></li>") : items) + "</ul></details></div>");
        keepalive();
    }
    send_chunk("<button type='submit'>Скачать TAR / Download TAR</button>"
               "<button type='submit' class='del' formaction='/delete' formmethod='POST' "
               "onclick=\"var n=document.querySelectorAll('input[name=folder]:checked').length;"
               "if(!n){alert('Отметьте папки / Select folders');return false}"
               "return confirm('Удалить выбранные папки ('+n+') со всем содержимым? Отменить будет нельзя. / "
               "Delete selected folders ('+n+') with all contents? This cannot be undone.')\">"
               "Удалить выбранные / Delete selected</button></form></body></html>");
    s_srv->sendContent("");
}

/* ---------------- отдача файлов ---------------- */
/* WebServer::streamFile()/sendContent() не повторяют запись: если TCP-буфер занят дольше таймаута,
   write() вернёт 0, кусок потеряется, а браузер будет ждать обещанные Content-Length байт.
   Здесь каждый кусок дописывается до конца; обрыв — только при отключении клиента или 15 с без прогресса. */
#define DL_CHUNK     4096
#define DL_STALL_MS  15000
static uint8_t s_buf[DL_CHUNK];

static bool send_all(WiFiClient &c, const uint8_t *d, size_t n)
{
    uint32_t last = millis();
    while (n > 0) {
        if (!c.connected()) return false;
        size_t w = c.write(d, n);
        if (w > 0) { d += w; n -= w; last = millis(); }
        else { if (millis() - last > DL_STALL_MS) return false; delay(2); }
    }
    return true;
}

/* ровно want байт из файла (если файл короче — добиваем нулями, чтобы не нарушить Content-Length) */
static bool send_file_bytes(WiFiClient &c, File &f, uint32_t want, uint32_t &sent)
{
    static uint32_t last_touch;
    while (want > 0) {
        size_t n = want < DL_CHUNK ? want : DL_CHUNK;
        size_t got = f ? f.read(s_buf, n) : 0;
        if (got < n) memset(s_buf + got, 0, n - got);
        if (!send_all(c, s_buf, n)) return false;
        want -= n; sent += n;
        if (millis() - last_touch > 2000) { keepalive(); last_touch = millis(); }
    }
    return true;
}

/* некоторые браузеры и менеджеры загрузок считают скачивание законченным только по закрытию соединения */
static void finish(WiFiClient &c) { delay(20); c.stop(); }

static void h_download()
{
    keepalive();
    String path = s_srv->arg("file");
    if (!is_log_file(path)) { s_srv->send(400, "text/plain; charset=utf-8", "Недопустимый путь / Invalid path"); return; }
    if (!hud_sd_ready())    { s_srv->send(503, "text/plain; charset=utf-8", "SD-карта не найдена / SD card not found"); return; }
    File f = SD.open(path, FILE_READ);
    if (!f || f.isDirectory()) { s_srv->send(404, "text/plain; charset=utf-8", "Файл не найден / File not found"); return; }
    uint32_t size = f.size(), sent = 0;
    s_srv->sendHeader("Content-Disposition", "attachment; filename=\"" + path.substring(17) + "\"");
    s_srv->setContentLength(size);
    s_srv->send(200, "application/octet-stream", "");
    uint32_t t0 = millis();
    WiFiClient client = s_srv->client();
    bool ok = send_file_bytes(client, f, size, sent);
    f.close();
    finish(client);
    uint32_t dt = millis() - t0;
    arduino_printf("[logs] %s: %s, %lu из %lu байт за %lu мс\n", path.c_str(), ok ? "OK" : "ОБРЫВ",
                   (unsigned long)sent, (unsigned long)size, (unsigned long)dt);
    keepalive();
}

/* ---------------- TAR (ustar, без сжатия) ---------------- */
static void tar_octal(uint8_t *h, int off, int len, unsigned long v)
{
    char b[16];
    snprintf(b, sizeof b, "%0*lo", len - 1, v);
    memcpy(h + off, b, len - 1);
    h[off + len - 1] = 0;
}

static void tar_header(uint8_t *h, const String &name, uint32_t size, uint32_t mtime)
{
    memset(h, 0, 512);
    strncpy((char *)h, name.c_str(), 100);
    tar_octal(h, 100, 8, 0644); tar_octal(h, 108, 8, 0); tar_octal(h, 116, 8, 0);
    tar_octal(h, 124, 12, size); tar_octal(h, 136, 12, mtime);
    memset(h + 148, ' ', 8);
    h[156] = '0';
    memcpy(h + 257, "ustar", 5); h[263] = '0'; h[264] = '0';
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += h[i];
    char b[8];
    snprintf(b, sizeof b, "%06o", sum);
    memcpy(h + 148, b, 6); h[154] = 0; h[155] = ' ';
}

struct TarItem { String path, name; uint32_t size, mtime; };

static void h_download_tar()
{
    keepalive();
    std::vector<String> dirs;
    for (int i = 0; i < s_srv->args(); i++)
        if (s_srv->argName(i) == "folder" && is_date_dir(s_srv->arg(i))) dirs.push_back(s_srv->arg(i));
    if (dirs.empty()) { s_srv->send(400, "text/plain; charset=utf-8", "Не выбрано ни одной папки / No folders selected"); return; }
    if (!hud_sd_ready()) { s_srv->send(503, "text/plain; charset=utf-8", "SD-карта не найдена / SD card not found"); return; }

    /* проход 1: список файлов и размеры -> точный размер архива (прогресс в браузере, без chunked) */
    std::vector<TarItem> items;
    uint64_t total = 1024;                                   /* два пустых блока в конце */
    for (const String &d : dirs)
        for (const FileItem &f : list_files(d)) {
            items.push_back({ d + "/" + f.name, d.substring(6) + "/" + f.name, f.size, f.mtime });
            total += 512 + ((uint64_t)(f.size + 511) / 512) * 512;
        }
    if (items.empty())          { s_srv->send(404, "text/plain; charset=utf-8", "В выбранных папках нет файлов / No files in the selected folders"); return; }
    if (total > 0xFFFFFFFFull)  { s_srv->send(413, "text/plain; charset=utf-8", "Слишком много (больше 4 ГБ), отметьте меньше папок / Too big (over 4 GB), select fewer folders"); return; }

    s_srv->sendHeader("Content-Disposition", "attachment; filename=\"hud_logs.tar\"");
    s_srv->setContentLength((size_t)total);
    s_srv->send(200, "application/x-tar", "");
    WiFiClient client = s_srv->client();
    uint32_t t0 = millis(), sent = 0;
    bool ok = true;

    /* проход 2: заголовок + ровно объявленный размер + выравнивание до 512 */
    for (TarItem &it : items) {
        uint8_t h[512];
        tar_header(h, it.name, it.size, it.mtime);
        if (!(ok = send_all(client, h, 512))) break;
        sent += 512;
        File f = SD.open(it.path, FILE_READ);                /* пропал файл — добьём нулями */
        ok = send_file_bytes(client, f, ((it.size + 511) / 512) * 512, sent);
        if (f) f.close();
        if (!ok) break;
    }
    if (ok) {
        memset(s_buf, 0, 1024);
        if ((ok = send_all(client, s_buf, 1024))) sent += 1024;
    }
    finish(client);
    arduino_printf("[logs] TAR (%u файлов): %s, %lu из %lu байт за %lu мс\n", (unsigned)items.size(), ok ? "OK" : "ОБРЫВ",
                   (unsigned long)sent, (unsigned long)total, (unsigned long)(millis() - t0));
    keepalive();
}

/* ---------------- удаление ---------------- */
/* удалять во время обхода каталога FAT нельзя: сначала собираем имена */
static bool remove_dir(const String &dir, unsigned &nfiles)
{
    bool ok = true;
    File d = SD.open(dir);
    if (!d || !d.isDirectory()) { if (d) d.close(); return false; }
    std::vector<String> names;
    for (File e = d.openNextFile(); e; e = d.openNextFile()) {
        if (!e.isDirectory()) names.push_back(base_name(e.name()));
        else ok = false;                                     /* вложенных папок быть не должно */
        e.close();
    }
    d.close();
    for (const String &n : names) {
        if (SD.remove(dir + "/" + n)) nfiles++; else ok = false;
        keepalive();
    }
    return SD.rmdir(dir) && ok;
}

static void h_delete()
{
    keepalive();
    if (!hud_sd_ready()) { s_srv->send(503, "text/plain; charset=utf-8", "SD-карта не найдена / SD card not found"); return; }
    unsigned nd = 0, nf = 0, bad = 0;
    for (int i = 0; i < s_srv->args(); i++) {
        if (s_srv->argName(i) != "folder") continue;
        String f = s_srv->arg(i);
        if (!is_date_dir(f)) { bad++; continue; }
        if (remove_dir(f, nf)) nd++; else bad++;
    }
    arduino_printf("[logs] удалено папок %u, файлов %u, ошибок %u\n", nd, nf, bad);
    String h = String(FPSTR(LOGS_HEAD)) + "<p>Удалено папок / Folders deleted: <b>" + String(nd) + "</b>, файлов / files: <b>" + String(nf) + "</b></p>";
    if (bad) h += "<p style='color:#ff6b6b'>Не удалось / Failed or skipped: " + String(bad) + "</p>";
    h += "<p><a href='/logs'>К списку / Back to the list</a></p></body></html>";
    s_srv->send(200, "text/html; charset=utf-8", h);
    keepalive();
}

void hud_ota_logs_attach(WebServer &srv, void (*touch_cb)(void))
{
    s_srv = &srv;
    s_keepalive = touch_cb;
    srv.on("/logs", HTTP_GET, h_logs);
    srv.on("/download", HTTP_GET, h_download);
    srv.on("/download-tar", HTTP_GET, h_download_tar);
    srv.on("/delete", HTTP_POST, h_delete);
}
