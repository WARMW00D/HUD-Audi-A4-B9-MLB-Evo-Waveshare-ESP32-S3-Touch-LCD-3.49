#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include "sim_map.h"

static CRITICAL_SECTION s_cs;
static int s_inited, s_port;
static SOCKET s_ls = INVALID_SOCKET;
static wchar_t s_html[MAX_PATH * 2];
static char s_ykey[160], s_gkey[160];
static struct { int loaded, valid, hdg_x10, speed, sat, fix; double lat, lon; unsigned t; } s_p;

void map_update(int loaded, int valid, double lat, double lon, int hdg_x10, int speed, int sat, int fix, unsigned t_ms)
{
    if (!s_inited) return;
    EnterCriticalSection(&s_cs);
    s_p.loaded = loaded; s_p.valid = valid; s_p.lat = lat; s_p.lon = lon; s_p.hdg_x10 = hdg_x10;
    s_p.speed = speed; s_p.sat = sat; s_p.fix = fix; s_p.t = t_ms;
    LeaveCriticalSection(&s_cs);
}

static void send_all(SOCKET c, const char *b, int n)
{
    while (n > 0) { int k = send(c, b, n, 0); if (k <= 0) return; b += k; n -= k; }
}
static void reply(SOCKET c, const char *status, const char *type, const char *body, int n)
{
    char h[256];
    int m = snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", status, type, n);
    send_all(c, h, m); send_all(c, body, n);
}
static void json_str(char *o, size_t cap, const char *s)      /* экранирование для JSON */
{
    size_t k = 0;
    for (; *s && k + 3 < cap; s++) { if (*s == '"' || *s == '\\') o[k++] = '\\'; if ((unsigned char)*s >= 32) o[k++] = *s; }
    o[k] = 0;
}

static void serve(SOCKET c)
{
    char rq[1024]; int n = recv(c, rq, sizeof rq - 1, 0);
    if (n <= 0) return;
    rq[n] = 0;
    if (strncmp(rq, "GET /pos", 8) == 0) {
        char b[400];
        EnterCriticalSection(&s_cs);
        int m = snprintf(b, sizeof b, "{\"loaded\":%d,\"valid\":%d,\"lat\":%.7f,\"lon\":%.7f,\"hdg\":%.1f,\"spd\":%d,\"sat\":%d,\"fix\":%d,\"t\":%u}",
                         s_p.loaded, s_p.valid, s_p.lat, s_p.lon, s_p.hdg_x10 < 0 ? -1.0 : s_p.hdg_x10 / 10.0, s_p.speed, s_p.sat, s_p.fix, s_p.t);
        LeaveCriticalSection(&s_cs);
        reply(c, "200 OK", "application/json", b, m);
    } else if (strncmp(rq, "GET /cfg", 8) == 0) {
        char y[320], g[320], b[800];
        json_str(y, sizeof y, s_ykey); json_str(g, sizeof g, s_gkey);
        int m = snprintf(b, sizeof b, "{\"yandex_key\":\"%s\",\"gis_key\":\"%s\"}", y, g);
        reply(c, "200 OK", "application/json", b, m);
    } else if (strncmp(rq, "GET / ", 6) == 0 || strncmp(rq, "GET /?", 6) == 0 || strncmp(rq, "GET /index", 10) == 0 || strncmp(rq, "GET /map", 8) == 0) {
        FILE *f = _wfopen(s_html, L"rb");
        if (!f) { const char *e = "map.html not found next to hud_sim.exe / map.html не найден рядом с hud_sim.exe"; reply(c, "404 Not Found", "text/plain; charset=utf-8", e, (int)strlen(e)); return; }
        fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
        char *buf = (char *)malloc((size_t)len + 1);
        if (buf && fread(buf, 1, (size_t)len, f) == (size_t)len) reply(c, "200 OK", "text/html; charset=utf-8", buf, (int)len);
        free(buf); fclose(f);
    } else {
        reply(c, "404 Not Found", "text/plain", "404", 3);
    }
}

static DWORD WINAPI srv(LPVOID p)
{
    (void)p;
    for (;;) {
        SOCKET c = accept(s_ls, NULL, NULL);
        if (c == INVALID_SOCKET) { Sleep(50); continue; }
        DWORD to = 2000; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
        serve(c);
        shutdown(c, SD_SEND); closesocket(c);
    }
    return 0;
}

int map_server_port(void) { return s_port; }

int map_server_start(int port, const wchar_t *html_path, const char *yandex_key, const char *gis_key)
{
    if (s_port) return s_port;
    WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w)) return 0;
    InitializeCriticalSection(&s_cs); s_inited = 1;
    wcsncpy(s_html, html_path, MAX_PATH * 2 - 1);
    strncpy(s_ykey, yandex_key ? yandex_key : "", sizeof s_ykey - 1);
    strncpy(s_gkey, gis_key ? gis_key : "", sizeof s_gkey - 1);
    for (int tries = 0; tries < 20; tries++, port++) {          /* занят порт — берём следующий */
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return 0;
        struct sockaddr_in a; memset(&a, 0, sizeof a);
        a.sin_family = AF_INET; a.sin_port = htons((u_short)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(s, (struct sockaddr *)&a, sizeof a) == 0 && listen(s, 8) == 0) {
            s_ls = s; s_port = port;
            CloseHandle(CreateThread(NULL, 0, srv, NULL, 0, NULL));
            return port;
        }
        closesocket(s);
    }
    return 0;
}
