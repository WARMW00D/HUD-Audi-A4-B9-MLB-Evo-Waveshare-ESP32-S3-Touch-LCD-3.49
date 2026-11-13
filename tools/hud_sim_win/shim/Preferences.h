/* Заглушка Preferences (NVS) для hud_settings.cpp: значения лежат в hud_sim.ini (секция [hud]).
   Так настройки меню HUD следуют за прошивкой сами — hud_settings.cpp собирается без правок. */
#pragma once
#include <stdint.h>
#include <stddef.h>
extern "C" int  sim_plat_cfg_get(const char *key, int def);
extern "C" void sim_plat_cfg_set(const char *key, int val);

class Preferences {
public:
    bool begin(const char *, bool = false) { return true; }
    void end() {}
    bool     getBool(const char *k, bool d = false)     { return sim_plat_cfg_get(k, d) != 0; }
    uint8_t  getUChar(const char *k, uint8_t d = 0)     { return (uint8_t)sim_plat_cfg_get(k, d); }
    int8_t   getChar(const char *k, int8_t d = 0)       { return (int8_t)sim_plat_cfg_get(k, d); }
    uint16_t getUShort(const char *k, uint16_t d = 0)   { return (uint16_t)sim_plat_cfg_get(k, d); }
    int16_t  getShort(const char *k, int16_t d = 0)     { return (int16_t)sim_plat_cfg_get(k, d); }
    uint32_t getUInt(const char *k, uint32_t d = 0)     { return (uint32_t)sim_plat_cfg_get(k, (int)d); }
    int32_t  getInt(const char *k, int32_t d = 0)       { return sim_plat_cfg_get(k, d); }
    size_t   putBool(const char *k, bool v)             { sim_plat_cfg_set(k, v); return 1; }
    size_t   putUChar(const char *k, uint8_t v)         { sim_plat_cfg_set(k, v); return 1; }
    size_t   putChar(const char *k, int8_t v)           { sim_plat_cfg_set(k, v); return 1; }
    size_t   putUShort(const char *k, uint16_t v)       { sim_plat_cfg_set(k, v); return 2; }
    size_t   putShort(const char *k, int16_t v)         { sim_plat_cfg_set(k, v); return 2; }
    size_t   putUInt(const char *k, uint32_t v)         { sim_plat_cfg_set(k, (int)v); return 4; }
    size_t   putInt(const char *k, int32_t v)           { sim_plat_cfg_set(k, v); return 4; }
};
