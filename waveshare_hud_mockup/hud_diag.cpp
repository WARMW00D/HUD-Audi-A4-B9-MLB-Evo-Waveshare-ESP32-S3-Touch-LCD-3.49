/*
  hud_diag.cpp — см. hud_diag.h
*/
#include <Arduino.h>
#include <Preferences.h>
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "hud_diag.h"
#include "hud_source.h"

extern "C" {
hud_diag_t g_diag;
}

extern "C" void hud_diag_save(void)
{
    g_diag.ver = HUD_DIAG_VER;
    g_diag.uptime_s = millis() / 1000;
    g_diag.heap_free  = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    g_diag.psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    g_diag.reset_reason = (uint8_t)esp_reset_reason();
    g_diag.src = (int8_t)hud_src_current();
    Preferences p;
    if (p.begin("hudd", false)) { p.putBytes("d", &g_diag, sizeof g_diag); p.end(); }
}

extern "C" bool hud_diag_load(hud_diag_t *out)
{
    Preferences p;
    if (!p.begin("hudd", true)) return false;
    bool ok = p.getBytesLength("d") == sizeof(hud_diag_t) && p.getBytes("d", out, sizeof(hud_diag_t)) == sizeof(hud_diag_t);
    p.end();
    return ok && out->ver == HUD_DIAG_VER;
}
