/*
  hud_settings.cpp — настройки HUD во флеше (NVS, пространство "hud")
  ------------------------------------------------------------------------------
  Язык, единицы, источник знаков, бар ускорения. Значения из hud_config.h —
  только для самого первого запуска (пока в NVS ничего нет).
*/
#include <Arduino.h>
#include <Preferences.h>
#include "hud_config.h"
#include "hud_mockup.h"
#include "hud_log.h"
#include "hud_sound.h"
#include "hud_light.h"

void hud_settings_load(void)
{
    Preferences p;
    if (!p.begin("hud", true)) {                          /* ещё не создано — значения по умолчанию */
        hud_set_lang(HUD_LANG); hud_set_units(HUD_UNITS);
        hud_set_psd(HUD_PSD_LIMITS); hud_set_vze(HUD_VZE_SIGNS); hud_set_accel_bar(HUD_ACCEL_BAR);
        hud_set_fuel_avg(HUD_FUEL_AVG); hud_set_gallons(HUD_VOLUME_GAL);
        hud_sound_set_volume(HUD_SOUND_VOLUME); hud_set_overspeed_tol(HUD_OVERSPEED_TOL_KMH);
        hud_light_set_enabled(HUD_LDR_ON);
        return;
    }
    hud_set_lang(p.getUChar("lang", HUD_LANG));
    hud_set_units(p.getUChar("units", HUD_UNITS));
    hud_set_psd(p.getBool("psd", HUD_PSD_LIMITS));
    hud_set_vze(p.getBool("vze", HUD_VZE_SIGNS));
    hud_set_accel_bar(p.getBool("accel", HUD_ACCEL_BAR));
    hud_set_fuel_avg(p.getBool("favg", HUD_FUEL_AVG));
    hud_set_gallons(p.getBool("gal", HUD_VOLUME_GAL));
    hud_sound_set_volume(p.getUChar("vol", HUD_SOUND_VOLUME));
    hud_set_overspeed_tol(p.getUChar("tol", HUD_OVERSPEED_TOL_KMH));
    hud_light_set_enabled(p.getBool("ldr", HUD_LDR_ON));
    hud_light_set_cal(p.getUShort("ldk", HUD_LDR_DARK_MV), p.getUShort("ldb", HUD_LDR_BRIGHT_MV));
    p.end();
    arduino_printf("[set] язык %s, %s, знаки VZE %s / PSD %s, бар ускорения %s, расход %s, звук %u, допуск %u км/ч, датчик света %s\n",
                   hud_get_lang() == HUD_LANG_EN ? "EN" : "RU",
                   hud_get_units() == HUD_UNITS_MI ? "мили" : "км",
                   hud_get_vze() ? "вкл" : "выкл", hud_get_psd() ? "вкл" : "выкл",
                   hud_get_accel_bar() ? "вкл" : "выкл", hud_get_fuel_avg() ? "средний" : "мгновенный",
                   (unsigned)hud_sound_get_volume(), (unsigned)hud_get_overspeed_tol(), hud_light_enabled() ? "вкл" : "выкл");
}

void hud_settings_save(void)
{
    Preferences p;
    if (!p.begin("hud", false)) return;
    p.putUChar("lang", hud_get_lang());
    p.putUChar("units", hud_get_units());
    p.putBool("psd", hud_get_psd());
    p.putBool("vze", hud_get_vze());
    p.putBool("accel", hud_get_accel_bar());
    p.putBool("favg", hud_get_fuel_avg());
    p.putBool("gal", hud_get_gallons());
    p.putUChar("vol", hud_sound_get_volume());
    p.putUChar("tol", hud_get_overspeed_tol());
    { int d, b; hud_light_get_cal(&d, &b);
      p.putBool("ldr", hud_light_enabled()); p.putUShort("ldk", (uint16_t)d); p.putUShort("ldb", (uint16_t)b); }
    p.end();
}
