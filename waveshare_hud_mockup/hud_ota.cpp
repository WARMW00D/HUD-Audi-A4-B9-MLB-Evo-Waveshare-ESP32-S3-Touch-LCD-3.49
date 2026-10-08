/*
  hud_ota.cpp — OTA по Wi-Fi: кнопка BOOT -> перезагрузка в режим обновления ->
  точка доступа + страница загрузки .bin (WebServer + Update из ядра ESP32).
  ------------------------------------------------------------------------------
  Почему через перезагрузку: в режиме обновления не работают BLE, TWAI и звук —
  Wi-Fi и загрузка получают всю память и процессор, а при обрыве старая прошивка
  остаётся нетронутой (Update переключает раздел только после успешной записи).
  Нужна таблица разделов с двумя приложениями (OTA), см. README.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include "hud_config.h"
#include "hud_data.h"
#include "hud_log.h"
#include "hud_sound.h"
#include "hud_ota.h"
extern "C" {
#include "hud_sounds.h"
}
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef HUD_OTA_PASS
#define HUD_OTA_PASS "hud12345"           /* не короче 8 символов (WPA2); свой — в secrets.h */
#endif

static bool         s_mode = false;
static HudOtaInfo   s_info;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static WebServer   *s_srv = nullptr;
static uint32_t     s_t0, s_restart_at;

extern "C" {

bool hud_ota_mode(void) { return s_mode; }

void hud_ota_get(HudOtaInfo *o)
{
    portENTER_CRITICAL(&s_mux);
    *o = s_info;
    portEXIT_CRITICAL(&s_mux);
}

bool hud_ota_check_boot(void)
{
    Preferences p;
    if (!p.begin("hud", false)) return false;
    bool on = p.getBool("ota", false);
    if (on) p.putBool("ota", false);           /* только один раз: следующая загрузка — обычная */
    p.end();
    s_mode = on;
    return on;
}

}

static void set_state(int st, int pct, const char *msg)
{
    portENTER_CRITICAL(&s_mux);
    s_info.state = st;
    if (pct >= 0) s_info.percent = pct;
    if (msg) { strncpy(s_info.msg, msg, sizeof s_info.msg - 1); s_info.msg[sizeof s_info.msg - 1] = 0; }
    portEXIT_CRITICAL(&s_mux);
}

/* ждём отпускания BOOT перед перезагрузкой: с зажатой GPIO0 сброс по питанию ушёл бы в режим загрузчика */
static void wait_boot_release(void)
{
    for (int i = 0; i < 100 && digitalRead(HUD_OTA_BTN_PIN) == LOW; i++) delay(30);
}

/* ---------------- обычный режим: кнопка BOOT ---------------- */
static void button_task(void *)
{
    uint32_t down = 0;
    pinMode(HUD_OTA_BTN_PIN, INPUT_PULLUP);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (digitalRead(HUD_OTA_BTN_PIN) != LOW) { down = 0; continue; }
        if (!down) { down = millis(); continue; }
        if (millis() - down < HUD_OTA_HOLD_MS) continue;
        down = 0;
        HudData d;
        hud_data_snapshot(&d);
        if ((d.valid & V_SPEED) && d.speed_kmh > HUD_MENU_MAX_KMH) {       /* на ходу — игнорируем */
            hud_log_write("[ota] BOOT на ходу — игнорирую\n");
            while (digitalRead(HUD_OTA_BTN_PIN) == LOW) vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        hud_log_write("[ota] BOOT удержан — перезагрузка в режим обновления\n");
        hud_sound_play(hud_snd_swa_beep, hud_snd_swa_beep_len);
        Preferences p;
        if (p.begin("hud", false)) { p.putBool("ota", true); p.end(); }
        vTaskDelay(pdMS_TO_TICKS(400));
        wait_boot_release();
        ESP.restart();
    }
}

extern "C" void hud_ota_start_button(void)
{
    xTaskCreatePinnedToCore(button_task, "hud_ota_btn", 3072, NULL, 1, NULL, 1);
}

/* ---------------- режим обновления ---------------- */
static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>HUD update</title>
<style>body{font-family:sans-serif;max-width:480px;margin:24px auto;padding:0 16px;background:#111;color:#eee}
h1{font-size:20px}input,button{font-size:16px;margin:8px 0}button{padding:10px 18px}
progress{width:100%;height:20px}#m{margin-top:12px;min-height:24px}small{color:#999}</style></head><body>
<h1>HUD — обновление прошивки / firmware update</h1>
<p><small>%INFO%</small></p>
<input type="file" id="f" accept=".bin"><br>
<button id="b" onclick="up()">Загрузить / Upload</button>
<progress id="p" value="0" max="100" style="display:none"></progress><div id="m"></div>
<script>
function up(){var f=document.getElementById('f').files[0],m=document.getElementById('m'),p=document.getElementById('p');
if(!f){m.textContent='Выберите .bin / choose a .bin';return}
var x=new XMLHttpRequest(),d=new FormData();d.append('fw',f,f.name);p.style.display='block';
x.upload.onprogress=function(e){if(e.lengthComputable)p.value=100*e.loaded/e.total};
x.onload=function(){m.textContent=x.status==200?'Готово, HUD перезагружается / Done, rebooting':'Ошибка / Error: '+x.responseText};
x.onerror=function(){m.textContent='Нет связи / Connection lost'};
document.getElementById('b').disabled=true;x.open('POST','/update');x.send(d)}
</script></body></html>)HTML";

static bool s_up_ok, s_up_started;
static size_t s_up_len;

static void on_upload()
{
    HTTPUpload &u = s_srv->upload();
    if (u.status == UPLOAD_FILE_START) {
        s_up_ok = false; s_up_started = true; s_up_len = 0;
        set_state(HUD_OTA_UPLOAD, 0, "");
        hud_log_write("[ota] приём файла\n");
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            set_state(HUD_OTA_ERROR, -1, Update.errorString());
            s_up_started = false;
        }
    } else if (u.status == UPLOAD_FILE_WRITE && s_up_started) {
        if (Update.write(u.buf, u.currentSize) != u.currentSize) {
            set_state(HUD_OTA_ERROR, -1, Update.errorString());
            Update.abort(); s_up_started = false;
        } else {
            s_up_len += u.currentSize;
            size_t tot = s_srv->clientContentLength();                 /* размер запроса ~ размеру файла */
            if (tot > 0) set_state(HUD_OTA_UPLOAD, (int)(((s_up_len * 100) / tot) > 99 ? 99 : ((s_up_len * 100) / tot)), NULL);
            s_t0 = millis();                                            /* пока идёт загрузка — таймаут не идёт */
        }
    } else if (u.status == UPLOAD_FILE_END && s_up_started) {
        if (Update.end(true)) { s_up_ok = true; set_state(HUD_OTA_DONE, 100, ""); }
        else set_state(HUD_OTA_ERROR, -1, Update.errorString());
        s_up_started = false;
    } else if (u.status == UPLOAD_FILE_ABORTED) {
        Update.abort(); s_up_started = false;
        set_state(HUD_OTA_ERROR, -1, "upload aborted");
    }
}

static void on_done()
{
    if (s_up_ok) {
        s_srv->send(200, "text/plain", "OK");
        s_restart_at = millis() + 2500;
    } else {
        HudOtaInfo i; hud_ota_get(&i);
        s_srv->send(500, "text/plain", i.msg[0] ? i.msg : "failed");
        set_state(HUD_OTA_WAIT, 0, NULL);
    }
}

static void ota_task(void *)
{
    s_srv = new WebServer(80);
    s_srv->on("/", HTTP_GET, []() {
        String p = FPSTR(PAGE);
        p.replace("%INFO%", String("build ") + __DATE__ + " " + __TIME__ + " · free " + String(ESP.getFreeSketchSpace() / 1024) + " KB");
        s_srv->send(200, "text/html; charset=utf-8", p);
    });
    s_srv->on("/update", HTTP_POST, on_done, on_upload);
    s_srv->onNotFound([]() { s_srv->sendHeader("Location", "/"); s_srv->send(302); });   /* captive-подобный редирект */
    s_srv->begin();
    s_t0 = millis();
    for (;;) {
        s_srv->handleClient();
        uint32_t now = millis();
        if (s_restart_at && (int32_t)(now - s_restart_at) >= 0) { hud_log_write("[ota] готово — перезагрузка\n"); ESP.restart(); }
        int left = (int)((HUD_OTA_TIMEOUT_MS - (now - s_t0)) / 1000);
        if (left < 0) { hud_log_write("[ota] таймаут — выход\n"); ESP.restart(); }
        portENTER_CRITICAL(&s_mux); s_info.left_s = left; portEXIT_CRITICAL(&s_mux);
        /* BOOT — выйти без обновления */
        static uint32_t dn;
        if (digitalRead(HUD_OTA_BTN_PIN) == LOW) { if (!dn) dn = now; else if (now - dn > 300) { hud_log_write("[ota] выход по BOOT\n"); wait_boot_release(); ESP.restart(); } }
        else dn = 0;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

extern "C" void hud_ota_run(void)
{
    pinMode(HUD_OTA_BTN_PIN, INPUT_PULLUP);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(HUD_OTA_SSID, HUD_OTA_PASS, 6, 0, 2);
    IPAddress ip = WiFi.softAPIP();
    portENTER_CRITICAL(&s_mux);
    memset(&s_info, 0, sizeof s_info);
    strncpy(s_info.ssid, HUD_OTA_SSID, sizeof s_info.ssid - 1);
    strncpy(s_info.pass, HUD_OTA_PASS, sizeof s_info.pass - 1);
    snprintf(s_info.ip, sizeof s_info.ip, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    s_info.left_s = HUD_OTA_TIMEOUT_MS / 1000;
    portEXIT_CRITICAL(&s_mux);
    hud_log_write("[ota] режим обновления: Wi-Fi ");
    hud_log_write(HUD_OTA_SSID);
    hud_log_write(", http://192.168.4.1\n");
    xTaskCreatePinnedToCore(ota_task, "hud_ota", 8192, NULL, 2, NULL, 1);
}
