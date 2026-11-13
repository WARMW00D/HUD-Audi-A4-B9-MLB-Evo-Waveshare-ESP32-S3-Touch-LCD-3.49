#!/usr/bin/env python3
"""
gen_min_images.py — картинки минималистичного режима HUD (LV_IMG_CF_ALPHA_4BIT, цвет задаётся в прошивке через img_recolor)
------------------------------------------------------------------------------
Иконка ACC (по присланному рисунку; координаты рисунка 1254 px, масштаб 0.135 -> 157x122 px):
  m_lane_l / m_lane_r — боковые полосы (цвет каждой свой: серый / жёлтый / зелёный),
  m_lead              — машина впереди (та же геометрия x0.652),
  m_arcs              — четыре радарные дуги,
  m_own               — своя машина.
Остальное: m_swa — красный треугольник Side Assist, m_cam — значок камеры со вспышкой (расстояние до камеры
рисует прошивка текстом).
Каждая картинка обрезана по рамке; смещение в рамке иконки — в hud_min_images.h.

Выход: waveshare_hud_mockup/hud_min_images.c / hud_min_images.h
Запуск из корня репозитория: python3 tools/gen_min_images.py   (нужен Pillow)
"""
import os
from PIL import Image, ImageDraw

TOOLS = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(TOOLS, "..", "waveshare_hud_mockup")

S4 = 4                         # сглаживание
OX, OY = 44, 154               # начало иконки в координатах рисунка
SC = WI = HI = K = 0           # масштаб рисунка и рамка иконки — задаются set_scale(): 0.135 (157x122, минимальный режим), 0.055 (64x50, полный)
def set_scale(sc):
    global SC, WI, HI, K
    SC = sc; WI, HI = round(1166 * sc), round(902 * sc); K = sc * S4
CX0 = 626                       # ось машины в координатах рисунка

def T(x, y): return ((x - OX) * K, (y - OY) * K)

def own_car(d, cx, top, s):
    """машина как на присланной картинке; s — масштаб относительно «своей» машины"""
    X = lambda x: T(cx + (x - CX0) * s, 0)[0]
    Y = lambda y: T(0, top + (y - 651) * s)[1]
    w = lambda v: max(1, int(round(v * s * K)))
    col = 255
    pts = [(X(CX0 - 250), Y(786)), (X(CX0 - 172), Y(666)), (X(CX0 + 172), Y(666)), (X(CX0 + 250), Y(786))]
    d.line(pts, fill=col, width=w(32), joint="curve")                       # кабина
    for (px, py) in pts:
        r = w(32) / 2; d.ellipse([px - r, py - r, px + r, py + r], fill=col)
    d.rounded_rectangle([X(289), Y(775), X(962), Y(807)], radius=w(16), fill=col)                 # пояс с зеркалами
    d.rounded_rectangle([X(314), Y(790), X(936), Y(978)], radius=w(55), outline=col, width=w(35)) # кузов
    d.rounded_rectangle([X(314), Y(864), X(471), Y(896)], radius=w(16), fill=col)                 # фары-«чёрточки»
    d.rounded_rectangle([X(780), Y(864), X(936), Y(896)], radius=w(16), fill=col)
    d.rounded_rectangle([X(314), Y(950), X(394), Y(1050)], radius=w(20), fill=col)                # колёса-«ножки»
    d.rounded_rectangle([X(856), Y(950), X(936), Y(1050)], radius=w(20), fill=col)

def layer(fn):
    im = Image.new("L", (WI * S4, HI * S4), 0)
    fn(ImageDraw.Draw(im))
    return im.resize((WI, HI), Image.LANCZOS)

P = lambda pts: [T(x, y) for x, y in pts]
def lane_l(d): d.polygon(P([(352, 313), (394, 313), (152, 1050), (50, 1050)]), fill=255)
def lane_r(d): d.polygon(P([(902, 313), (860, 313), (1102, 1050), (1204, 1050)]), fill=255)
def lead(d): own_car(d, 626, 160, 0.652)
def own(d): own_car(d, 626, 651, 1.0)
def arcs(d):
    for apex, half, drop, th in ((445, 209, 40, 15), (498, 182, 32, 15), (550, 155, 26, 14), (598, 118, 20, 12)):
        line = [T(626 + half * u, apex + drop * u * u) for u in [i / 24 * 2 - 1 for i in range(25)]]
        d.line(line, fill=255, width=max(1, int(th * K)), joint="curve")

def swa_tri(w=26, h=22):
    im = Image.new("L", (w * 4, h * 4), 0); d = ImageDraw.Draw(im)
    d.polygon([(w * 2, 0), (0, h * 4 - 1), (w * 4 - 1, h * 4 - 1)], fill=255)
    return im.resize((w, h), Image.LANCZOS)

def cam_glyph(gw=46, gh=40):
    """значок камеры со вспышкой: корпус, объектив (чёрная дыра), искра"""
    k = 4
    g = Image.new("L", (gw * k, gh * k), 0); d = ImageDraw.Draw(g)
    X = lambda v: v * gw * k; Y = lambda v: v * gh * k
    d.rounded_rectangle([X(0.04), Y(0.34), X(0.86), Y(0.88)], radius=int(Y(0.10)), fill=255)
    d.rounded_rectangle([X(0.18), Y(0.20), X(0.40), Y(0.38)], radius=int(Y(0.05)), fill=255)
    cx, cy, r = X(0.45), Y(0.62), Y(0.22)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=0)
    r2 = r * 0.66; d.ellipse([cx - r2, cy - r2, cx + r2, cy + r2], fill=255)
    r3 = r * 0.30; d.ellipse([cx - r3, cy - r3, cx + r3, cy + r3], fill=0)
    sx, sy, R, rr = X(0.88), Y(0.20), Y(0.20), Y(0.045)
    d.polygon([(sx, sy - R), (sx + rr, sy - rr), (sx + R, sy), (sx + rr, sy + rr), (sx, sy + R), (sx - rr, sy + rr), (sx - R, sy), (sx - rr, sy - rr)], fill=255)
    return g.resize((gw, gh), Image.LANCZOS)

def pack4(im):
    w, h = im.size
    px = im.tobytes()
    out = bytearray()
    for y in range(h):
        row = px[y * w:(y + 1) * w]
        for x in range(0, w, 2):
            a = (row[x] + 8) // 17
            b = (row[x + 1] + 8) // 17 if x + 1 < w else 0
            out.append((min(a, 15) << 4) | min(b, 15))
    return bytes(out)

def emit(f, h, pre, items):
    total = 0
    for n, im, in_box in items:
        bb = im.getbbox() or (0, 0, 1, 1)
        c = im.crop(bb)
        data = pack4(c); total += len(data)
        rows = [", ".join(f"0x{b:02x}" for b in data[i:i + 24]) for i in range(0, len(data), 24)]
        f.write(f"static const uint8_t {n}_map[] = {{\n    " + ",\n    ".join(rows) + "\n};\n")
        f.write(f"const lv_img_dsc_t img_{n} = {{\n    .header.cf = LV_IMG_CF_ALPHA_4BIT, .header.always_zero = 0, .header.reserved = 0,\n"
                f"    .header.w = {c.width}, .header.h = {c.height}, .data_size = {len(data)}, .data = {n}_map,\n}};\n\n")
        h.write(f"extern const lv_img_dsc_t img_{n};\n")
        if in_box: h.write(f"#define {n.upper()}_X {bb[0]}\n#define {n.upper()}_Y {bb[1]}\n")
    return total

def acc_set(pre):
    return [(pre + "lane_l", layer(lane_l), True), (pre + "lane_r", layer(lane_r), True), (pre + "lead", layer(lead), True),
            (pre + "arcs", layer(arcs), True), (pre + "own", layer(own), True)]

def main():
    total = 0
    with open(os.path.join(OUT, "hud_min_images.c"), "w") as f, open(os.path.join(OUT, "hud_min_images.h"), "w") as h:
        f.write("/* Сгенерировано tools/gen_min_images.py — не править вручную */\n")
        f.write('#define LV_LVGL_H_INCLUDE_SIMPLE\n#include "lvgl.h"\n#include "hud_min_images.h"\n\n')
        h.write("/* Сгенерировано tools/gen_min_images.py — не править вручную */\n#ifndef HUD_MIN_IMAGES_H\n#define HUD_MIN_IMAGES_H\n#include \"lvgl.h\"\n\n")
        set_scale(0.135)
        h.write(f"/* иконка ACC минимального режима: рамка {WI} x {HI}; X/Y — смещение обрезанной картинки в рамке */\n#define M_ACC_W {WI}\n#define M_ACC_H {HI}\n")
        mset = acc_set("m_")
        total += emit(f, h, "m_", mset + [("m_swa", swa_tri(), False), ("m_cam", cam_glyph(), False)])
        set_scale(0.055)
        h.write(f"\n/* та же иконка для полного режима: рамка {WI} x {HI} */\n#define F_ACC_W {WI}\n#define F_ACC_H {HI}\n")
        fset = acc_set("f_")
        total += emit(f, h, "f_", fset)
        h.write("\n#endif\n")
    sheet = Image.new("RGB", (157 + 64 + 40, 140), (0, 0, 0))
    for items, x, size in ((mset, 10, (157, 122)), (fset, 180, (64, 50))):
        base = Image.new("RGB", size, (0, 0, 0))
        for (n, im, ib), col in zip(items, [(176, 180, 190), (176, 180, 190), (46, 230, 74), (46, 230, 74), (46, 230, 74)]):
            base.paste(Image.new("RGB", im.size, col), (0, 0), im)
        sheet.paste(base, (x, 10))
    sheet.save(os.path.join(TOOLS, "preview_min.png"))
    print(f"данных {total} байт")

if __name__ == "__main__":
    main()
