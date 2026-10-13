/*
  cam_replay.c — проверка cam_alert на ПК: проигрывает лог снифера (can_log_*.txt)
  против cams.bin и печатает, когда HUD показал бы камеру и с какого расстояния.
  Сборка: gcc -O2 -o cam_replay tools/cam_replay.c waveshare_hud_mockup/cam_alert.c -Iwaveshare_hud_mockup -lm
  Запуск: ./cam_replay cams.bin can_log_0004.txt [type_mask_hex]
*/
#include "cam_alert.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t sg(const uint8_t *d, int s, int l)
{
    uint64_t r = 0; for (int i = 7; i >= 0; i--) r = (r << 8) | d[i];
    return (uint32_t)((r >> s) & ((1ULL << l) - 1));
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "cams.bin log [mask]\n"); return 1; }
    FILE *f = fopen(argv[1], "rb"); if (!f) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n); if (fread(buf, 1, n, f) != (size_t)n) return 1; fclose(f);
    if (!cam_load(buf, n)) { fprintf(stderr, "bad cams.bin\n"); return 1; }
    cam_cfg_t c = { 12.0f, 200.0f, 600.0f, 35.0f, 50.0f, argc > 3 ? (uint16_t)strtol(argv[3], 0, 16) : 0x001 };
    cam_config(&c);
    printf("база: %u камер, от %06u\n", cam_count(), cam_date());

    FILE *l = fopen(argv[2], "r"); if (!l) return 1;
    char ln[256]; double lat = 0, lon = 0; float hdg = 999, spd = 0; int fix = 0, sat = 0; double hdop = 99;
    uint32_t t = 0; uint32_t cur = 0xFFFFFFFFu; float mind = 1e9; int events = 0; uint32_t cur_t0 = 0;
    while (fgets(ln, sizeof ln, l)) {
        unsigned id, dlc; uint8_t d[8] = {0}; char ty;
        unsigned b[8]; int k = sscanf(ln, "%u %c %x %u %x %x %x %x %x %x %x %x", &t, &ty, &id, &dlc, b, b+1, b+2, b+3, b+4, b+5, b+6, b+7);
        if (k < 5 || (ty != 'S' && ty != 'X')) continue;
        for (unsigned i = 0; i < 8 && i + 4 < (unsigned)k; i++) d[i] = (uint8_t)b[i];
        if (ty == 'S' && id == 0x486) { lat = sg(d, 0, 27) / 1e6; lon = sg(d, 27, 28) / 1e6; sat = sg(d, 57, 5); fix = sg(d, 62, 2); }
        else if (ty == 'S' && id == 0x484) { double h = sg(d, 50, 12) * 0.1; hdg = h < 360 ? (float)h : 999; hdop = sg(d, 20, 10) * 0.025; }
        else if (ty == 'S' && id == 0x30B) spd = sg(d, 24, 9) * 0.0f + sg(d, 24, 9);
        else continue;
        if (ty == 'S' && id == 0x486) {
            int ok = fix == 3 && sat >= 4 && hdop <= 4.0 && hdg < 360 && spd >= 15;
            cam_hit_t h; int hit = ok && cam_query(lat, lon, hdg, spd, &h);
            if (hit && h.idx != cur) {
                if (cur != 0xFFFFFFFFu) printf("   ... подошли на %.0f м (пик)\n", mind);
                cur = h.idx; mind = h.dist_m; cur_t0 = t; events++;
                printf("t=%7.1f c  КАМЕРА #%u  лим %3u  с %.0f м  v=%.0f  курс %.0f/кам %d  (%.5f,%.5f)\n", t / 1000.0, h.idx, h.limit, h.dist_m, spd, hdg, (int)h.dir_deg, lat, lon);
            } else if (hit && h.idx == cur) { if (h.dist_m < mind) mind = h.dist_m; }
            else if (!hit && cur != 0xFFFFFFFFu) { printf("   ... прошли, пик %.0f м, держали %.1f с\n", mind, (t - cur_t0) / 1000.0); cur = 0xFFFFFFFFu; }
        }
    }
    printf("событий: %d\n", events);
    return 0;
}
