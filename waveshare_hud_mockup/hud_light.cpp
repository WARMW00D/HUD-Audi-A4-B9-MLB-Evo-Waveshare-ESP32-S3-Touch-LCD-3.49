/*
  hud_light.cpp — фоторезистор на GPIO HUD_LDR_PIN (ADC1), запасной датчик света
  ------------------------------------------------------------------------------
  Напряжение на делителе усредняется (8 отсчётов) и сглаживается. Калибровка —
  два значения: «темно» (датчик закрыт) и «светло» (яркий фонарик); между ними
  освещённость считается линейно по напряжению (0…1) и переводится в «лк» так,
  чтобы общая логарифмическая формула яркости из hud_mockup.c дала то же t.
  Работает с любой полярностью делителя: если светло < темно, формула сама
  разворачивается.
*/
#include <Arduino.h>
#include <math.h>
#include "hud_config.h"
#include "hud_light.h"

static bool  s_enabled = HUD_LDR_ON;
static bool  s_force;
static int   s_dark = HUD_LDR_DARK_MV, s_bright = HUD_LDR_BRIGHT_MV;
static float s_mv = -1;
static bool  s_init;

extern "C" {

void hud_light_force(bool on) { s_force = on; }
bool hud_light_enabled(void) { return s_enabled; }
void hud_light_set_enabled(bool on) { s_enabled = on; }
void hud_light_get_cal(int *d, int *b) { if (d) *d = s_dark; if (b) *b = s_bright; }
void hud_light_set_cal(int d, int b) { s_dark = d; s_bright = b; }
int  hud_light_mv(void) { return s_mv < 0 ? -1 : (int)(s_mv + 0.5f); }

bool hud_light_cal_valid(int d, int b) { return abs(b - d) >= HUD_LDR_MIN_SPAN_MV; }

void hud_light_poll(void)
{
    if (!s_enabled && !s_force) { s_mv = -1; return; }
    if (!s_init) {
        pinMode(HUD_LDR_PIN, INPUT);
        analogSetPinAttenuation(HUD_LDR_PIN, ADC_11db);
        s_init = true;
    }
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(HUD_LDR_PIN);
    float v = sum / 8.0f;
    s_mv = (s_mv < 0) ? v : s_mv + (v - s_mv) * 0.2f;      /* ~0.25 с при шаге 50 мс */
}

int hud_light_lux(void)
{
    if (!s_enabled || s_mv < 0 || !hud_light_cal_valid(s_dark, s_bright)) return -1;
    float t = (s_mv - s_dark) / (float)(s_bright - s_dark);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float lo = log10f(HUD_LIGHT_LUX_DARK + 10.0f), hi = log10f(HUD_LIGHT_LUX_BRIGHT + 10.0f);
    return (int)(powf(10.0f, lo + t * (hi - lo)) - 10.0f + 0.5f);
}

}
