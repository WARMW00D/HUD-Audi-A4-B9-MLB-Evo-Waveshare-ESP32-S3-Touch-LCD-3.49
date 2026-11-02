/*
  hud_anim.c — см. hud_anim.h. Формат файла описан в tools/gen_arrow_anim.py.
*/
#include "hud_anim.h"
#include <string.h>
#include <stdlib.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define ANIM_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define ANIM_ALLOC(n) malloc(n)
#endif

#define PAL_BYTES 64                     /* 16 цветов x 4 байта */

static const uint8_t *s_buf;
static size_t         s_len;
static uint16_t       s_w, s_h, s_nanim;
static uint32_t       s_nframes;
static const uint8_t *s_pal, *s_anim, *s_offs;
static size_t         s_raw;             /* размер распакованного кадра, байт */
static uint8_t       *s_dec[2];          /* [палитра][пиксели], две штуки по очереди — чтобы LVGL не читал буфер, который пишем */
static lv_img_dsc_t   s_dsc[2];
static int            s_cur;

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* блок LZ4 (без заголовка кадра) -> dst; возвращает длину или -1 */
static int lz4_dec(const uint8_t *src, int slen, uint8_t *dst, int dcap)
{
    const uint8_t *ip = src, *ie = src + slen;
    uint8_t *op = dst, *oe = dst + dcap;
    while (ip < ie) {
        unsigned tok = *ip++;
        unsigned ll = tok >> 4;
        if (ll == 15) { unsigned s; do { if (ip >= ie) return -1; s = *ip++; ll += s; } while (s == 255); }
        if (ll > (unsigned)(ie - ip) || ll > (unsigned)(oe - op)) return -1;
        memcpy(op, ip, ll); op += ll; ip += ll;
        if (ip >= ie) break;                                   /* последняя последовательность — только литералы */
        if (ie - ip < 2) return -1;
        unsigned off = (unsigned)ip[0] | ((unsigned)ip[1] << 8); ip += 2;
        if (off == 0 || off > (unsigned)(op - dst)) return -1;
        unsigned ml = tok & 15;
        if (ml == 15) { unsigned s; do { if (ip >= ie) return -1; s = *ip++; ml += s; } while (s == 255); }
        ml += 4;
        if (ml > (unsigned)(oe - op)) return -1;
        const uint8_t *m = op - off;
        while (ml--) *op++ = *m++;                             /* копия с перекрытием — побайтно */
    }
    return (int)(op - dst);
}

bool hud_anim_ready(void) { return s_buf != NULL; }

bool hud_anim_set_data(const uint8_t *buf, size_t len)
{
    s_buf = NULL;
    if (!buf || len < 20 + PAL_BYTES || memcmp(buf, "ANM1", 4) != 0) return false;
    uint16_t w = rd16(buf + 4), h = rd16(buf + 6), n = rd16(buf + 8);
    if (w == 0 || h == 0 || (w & 1) || n == 0 || rd32(buf + 12) != len) return false;
    const uint8_t *pal = buf + 20, *anim = pal + PAL_BYTES;
    size_t offs_at = 20 + PAL_BYTES + (size_t)n * 32;
    if (offs_at > len) return false;
    uint32_t total = 0;
    for (int i = 0; i < n; i++) {
        uint32_t last = rd32(anim + i * 32 + 28) + rd16(anim + i * 32 + 24);
        if (last > total) total = last;
    }
    if (offs_at + (size_t)total * 4 > len) return false;
    size_t raw = (size_t)((w + 1) / 2) * h;
    for (int i = 0; i < 2; i++) {
        if (!s_dec[i]) s_dec[i] = (uint8_t *)ANIM_ALLOC(PAL_BYTES + raw);
        if (!s_dec[i]) return false;
        memcpy(s_dec[i], pal, PAL_BYTES);
        memset(&s_dsc[i], 0, sizeof s_dsc[i]);
        s_dsc[i].header.cf = LV_IMG_CF_INDEXED_4BIT;
        s_dsc[i].header.w = w; s_dsc[i].header.h = h;
        s_dsc[i].data_size = (uint32_t)(PAL_BYTES + raw);
        s_dsc[i].data = s_dec[i];
    }
    s_buf = buf; s_len = len; s_w = w; s_h = h; s_nanim = n; s_nframes = total;
    s_pal = pal; s_anim = anim; s_offs = buf + offs_at; s_raw = raw;
    return true;
}

int hud_anim_find(const char *name)
{
    if (!s_buf || !name) return -1;
    for (int i = 0; i < s_nanim; i++)
        if (strncmp((const char *)s_anim + i * 32, name, 24) == 0) return i;
    return -1;
}

int hud_anim_frames(int anim)
{
    if (!s_buf || anim < 0 || anim >= s_nanim) return 0;
    return rd16(s_anim + anim * 32 + 24);
}

const lv_img_dsc_t *hud_anim_frame(int anim, int frame)
{
    int n = hud_anim_frames(anim);
    if (n == 0) return NULL;
    if (frame < 0) frame = 0;
    if (frame >= n) frame = n - 1;
    uint32_t idx = rd32(s_anim + anim * 32 + 28) + (uint32_t)frame;
    if (idx >= s_nframes) return NULL;
    uint32_t off = rd32(s_offs + (size_t)idx * 4);
    if (off + 2 > s_len) return NULL;
    uint16_t clen = rd16(s_buf + off);
    if (off + 2 + clen > s_len) return NULL;
    int k = s_cur ^ 1;
    if (lz4_dec(s_buf + off + 2, clen, s_dec[k] + PAL_BYTES, (int)s_raw) != (int)s_raw) return NULL;
    s_cur = k;
    return &s_dsc[k];
}
