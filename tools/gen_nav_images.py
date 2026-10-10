#!/usr/bin/env python3
"""
gen_nav_images.py — стрелки манёвров навигации для HUD (LV_IMG_CF_ALPHA_4BIT)
------------------------------------------------------------------------------
Рисует стрелки по MainElement + Direction из BAP Navigation_SD:
  - поворот (Turn / PrepareTurn / неизвестный элемент) — 16 направлений по 22.5°
  - кольцо (Roundabout) — 16 направлений съезда, движение против часовой (правостороннее)
  - прямо, съезд с магистрали L/R, развилка L/R, разворот L/R, финиш
Яркость = альфа (фон чёрный): стрелка — полная яркость, дорога-контекст — серая.
Цвет задаётся в прошивке через img_recolor.

Direction: 0x00 прямо, 0x40 налево, 0x80 назад, 0xC0 направо (360/256 град,
против часовой). Сектор = ((dir + 8) >> 4) & 15.

Выход: waveshare_hud_mockup/hud_nav_images.c / hud_nav_images.h + tools/preview_nav.png
Запуск из корня репозитория: python3 tools/gen_nav_images.py   (нужен Pillow)
"""
import math, os
from PIL import Image, ImageDraw, ImageChops, ImageFilter

S = 8
W, H = 192, 116                 # область стрелки (ниже — строка дистанции)
TOOLS = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(TOOLS, "..", "waveshare_hud_mockup")      # папка скетча

ARROW_W = 15                    # толщина стрелки
HEAD_W, HEAD_L = 36, 22         # наконечник
ROAD_W = 22                     # дорога-контекст
ROAD_A = 70                     # яркость дороги (для маски стрелки не используется — дорога идёт отдельным слоем)
ARROW_A = 255                   # яркость стрелки при рисовании (255 — слой стрелки; ROAD_FILL — слой дороги)
ROAD_FILL = 100                 # слой дороги: заливка (серый на чёрном фоне HUD)
ROAD_BORDER = 2                 # слой дороги: белая кайма, px
CX, JY, BY = W / 2, 60, H - 4   # центр, перекрёсток, низ

def cv():
    return Image.new("L", (W * S, H * S), 0)

def P(pts):
    return [(x * S, y * S) for x, y in pts]

def vec(angle_deg):
    """Угол против часовой от 'вверх' -> вектор в экранных координатах."""
    a = math.radians(angle_deg)
    return -math.sin(a), -math.cos(a)

def line(d, pts, w, fill=None):
    if fill is None: fill = ARROW_A
    d.line(P(pts), fill=fill, width=int(w * S), joint="curve")
    for x, y in (pts[0], pts[-1]):
        r = w / 2
        d.ellipse([(x - r) * S, (y - r) * S, (x + r) * S, (y + r) * S], fill=fill)

def head(d, tip, v, fill=None):
    if fill is None: fill = ARROW_A
    vx, vy = v
    nx, ny = -vy, vx
    bx, by = tip[0] - vx * HEAD_L, tip[1] - vy * HEAD_L
    d.polygon(P([tip, (bx + nx * HEAD_W / 2, by + ny * HEAD_W / 2), (bx - nx * HEAD_W / 2, by - ny * HEAD_W / 2)]), fill=fill)

def arrow_path(d, pts):
    """Ломаная со скруглёнными стыками, последний отрезок — к наконечнику."""
    (x1, y1), (x2, y2) = pts[-2], pts[-1]
    L = math.hypot(x2 - x1, y2 - y1)
    v = ((x2 - x1) / L, (y2 - y1) / L)
    body_end = (x2 - v[0] * (HEAD_L - 2), y2 - v[1] * (HEAD_L - 2))
    d.line(P(pts[:-1] + [body_end]), fill=ARROW_A, width=int(ARROW_W * S), joint="curve")
    r = ARROW_W / 2
    x0, y0 = pts[0]
    d.ellipse([(x0 - r) * S, (y0 - r) * S, (x0 + r) * S, (y0 + r) * S], fill=ARROW_A)
    for (px, py) in pts[1:-1]:
        d.ellipse([(px - r) * S, (py - r) * S, (px + r) * S, (py + r) * S], fill=ARROW_A)
    head(d, pts[-1], v)

def fit_len(ox, oy, v, margin=4):
    """Длина луча из (ox,oy) по v до края области (с запасом под наконечник)."""
    ts = []
    for c0, dv, lo, hi in ((ox, v[0], margin + HEAD_W / 2, W - margin - HEAD_W / 2),
                           (oy, v[1], margin, H - margin)):
        if dv > 1e-6:  ts.append((hi - c0) / dv)
        if dv < -1e-6: ts.append((lo - c0) / dv)
    return min(ts) if ts else 60

# ------------------------------------------------------------------ манёвры ---
def img_turn(sector):
    ang = sector * 22.5
    im = cv(); d = ImageDraw.Draw(im)
    v = vec(ang)
    # дорога: прямо через перекрёсток + выбранное направление
    line(d, [(CX, BY + 6), (CX, 4)], ROAD_W, ROAD_A)
    Lr = min(fit_len(CX, JY, v, 0), 70)
    line(d, [(CX, JY), (CX + v[0] * Lr, JY + v[1] * Lr)], ROAD_W, ROAD_A)
    if sector == 0:
        arrow_path(d, [(CX, BY), (CX, 8)])
    elif sector == 8:
        return img_uturn(left=True)
    else:
        # крутые повороты: 7/9 (157.5 град) рисуем разворотом, 6/10 (135 град) —
        # со стойкой, смещённой в сторону от обратного луча
        if sector in (7, 9):
            return img_uturn(left=(sector == 7))
        if sector in (6, 10):
            sx = 1 if sector == 6 else -1
            x0, jy = CX + sx * 24, 34
            line(d, [(x0, jy), (x0 + v[0] * 70, jy + v[1] * 70)], ROAD_W, ROAD_A)
            line(d, [(x0, BY + 6), (x0, 4)], ROAD_W, ROAD_A)
            arrow_path(d, [(x0, BY), (x0, jy), (x0 + v[0] * 66, jy + v[1] * 66)])
        else:
            L = min(fit_len(CX, JY, v), 58)
            arrow_path(d, [(CX, BY), (CX, JY), (CX + v[0] * L, JY + v[1] * L)])
    return im

def img_uturn(left=True):
    im = cv(); d = ImageDraw.Draw(im)
    s = -1 if left else 1
    R = 20
    top = 30
    x2 = CX + s * 2 * R
    line(d, [(CX, BY + 6), (CX, 4)], ROAD_W, ROAD_A)
    line(d, [(x2, BY + 6), (x2, 4)], ROAD_W, ROAD_A)
    # стойка вверх, дуга, вниз
    d.line(P([(CX, BY), (CX, top + R)]), fill=ARROW_A, width=int(ARROW_W * S))
    r = ARROW_W / 2
    d.ellipse([(CX - r) * S, (BY - r) * S, (CX + r) * S, (BY + r) * S], fill=ARROW_A)
    cx = CX + s * R
    box = [(cx - R - ARROW_W / 2) * S, (top - ARROW_W / 2) * S, (cx + R + ARROW_W / 2) * S, (top + 2 * R + ARROW_W / 2) * S]
    d.ellipse(box, fill=ARROW_A)
    inner = [(cx - R + ARROW_W / 2) * S, (top + ARROW_W / 2) * S, (cx + R - ARROW_W / 2) * S, (top + 2 * R - ARROW_W / 2) * S]
    d.ellipse(inner, fill=0)
    d.rectangle([(cx - R - ARROW_W) * S, (top + R) * S, (cx + R + ARROW_W) * S, (top + 2 * R + ARROW_W) * S], fill=0)
    d.line(P([(CX, top + R), (CX, BY)]), fill=ARROW_A, width=int(ARROW_W * S))
    d.ellipse([(CX - r) * S, (BY - r) * S, (CX + r) * S, (BY + r) * S], fill=ARROW_A)
    tip_y = 86
    d.line(P([(x2, top + R), (x2, tip_y - HEAD_L + 2)]), fill=ARROW_A, width=int(ARROW_W * S))
    head(d, (x2, tip_y), (0, 1))
    return im

def img_roundabout(sector):
    ang = sector * 22.5
    im = cv(); d = ImageDraw.Draw(im)
    rc, R = (CX, 46), 29
    RW = 11                                   # толщина стрелки на кольце
    # все съезды серым: 4 основных луча + кольцо
    d.ellipse([(rc[0] - R - ROAD_W / 2) * S, (rc[1] - R - ROAD_W / 2) * S,
               (rc[0] + R + ROAD_W / 2) * S, (rc[1] + R + ROAD_W / 2) * S], fill=ROAD_A)
    d.ellipse([(rc[0] - R + ROAD_W / 2) * S, (rc[1] - R + ROAD_W / 2) * S,
               (rc[0] + R - ROAD_W / 2) * S, (rc[1] + R - ROAD_W / 2) * S], fill=0)
    line(d, [(CX, BY + 6), (CX, rc[1] + R)], ROAD_W, ROAD_A)
    v = vec(ang)
    ex0 = (rc[0] + v[0] * R, rc[1] + v[1] * R)
    Le = min(fit_len(ex0[0], ex0[1], v), 30)
    line(d, [ex0, (ex0[0] + v[0] * (Le + 6), ex0[1] + v[1] * (Le + 6))], ROAD_W, ROAD_A)
    # путь стрелки: въезд снизу, по кольцу против часовой (визуально), съезд
    entry = (CX, rc[1] + R)
    d.line(P([(CX, BY), entry]), fill=ARROW_A, width=int(ARROW_W * S))
    r = ARROW_W / 2
    d.ellipse([(CX - r) * S, (BY - r) * S, (CX + r) * S, (BY + r) * S], fill=ARROW_A)
    # экранный угол PIL: 0 = вправо (3 ч), растёт по часовой. Въезд = 90 (6 ч).
    # Выход в точке по направлению ang: экранный угол = 270 - ang (против часовой => уменьшаем)
    start, end = 90, (270 - ang) % 360
    # движение визуально против часовой: от 90 вниз к 0 и дальше -> дуга [end, start] по часовой в PIL
    span_start, span_end = end, start
    if span_start >= span_end: span_start -= 360
    if ang % 360 == 180: span_start, span_end = -270, 90   # полный круг (разворот)
    box = [(rc[0] - R) * S, (rc[1] - R) * S, (rc[0] + R) * S, (rc[1] + R) * S]
    box = [(rc[0] - R - RW / 2) * S, (rc[1] - R - RW / 2) * S, (rc[0] + R + RW / 2) * S, (rc[1] + R + RW / 2) * S]
    d.arc(box, span_start, span_end, fill=ARROW_A, width=int(RW * S))
    for a in (span_start, span_end):
        ar = math.radians(a)
        px, py = rc[0] + R * math.cos(ar), rc[1] + R * math.sin(ar)
        d.ellipse([(px - RW / 2) * S, (py - RW / 2) * S, (px + RW / 2) * S, (py + RW / 2) * S], fill=ARROW_A)
    tip = (ex0[0] + v[0] * Le, ex0[1] + v[1] * Le)
    body_end = (tip[0] - v[0] * (HEAD_L - 2), tip[1] - v[1] * (HEAD_L - 2))
    d.line(P([ex0, body_end]), fill=ARROW_A, width=int(ARROW_W * S))
    head(d, tip, v)
    return im

def img_exit(right=True):
    im = cv(); d = ImageDraw.Draw(im)
    s = 1 if right else -1
    xm = CX - s * 14
    line(d, [(xm, BY + 6), (xm, 4)], ROAD_W, ROAD_A)
    pts = [(xm, BY), (xm, 70), (xm + s * 30, 38), (xm + s * 44, 12)]
    line(d, [(xm, 70), (xm + s * 30, 38), (xm + s * 44, 4)], ROAD_W, ROAD_A)
    arrow_path(d, pts)
    return im

def img_fork(right=True):
    im = cv(); d = ImageDraw.Draw(im)
    s = 1 if right else -1
    line(d, [(CX, BY + 6), (CX, 64), (CX - 36, 24), (CX - 44, 4)], ROAD_W, ROAD_A)
    line(d, [(CX, 64), (CX + 36, 24), (CX + 44, 4)], ROAD_W, ROAD_A)
    arrow_path(d, [(CX, BY), (CX, 64), (CX + s * 34, 26), (CX + s * 42, 8)])
    return im

def img_straight():
    return img_turn(0)

def img_arrived():
    im = cv(); d = ImageDraw.Draw(im)
    # метка-капля с отверстием
    cx, cy, r = CX, 42, 24
    d.ellipse([(cx - r) * S, (cy - r) * S, (cx + r) * S, (cy + r) * S], fill=ARROW_A)
    d.polygon(P([(cx - r * 0.82, cy + r * 0.55), (cx + r * 0.82, cy + r * 0.55), (cx, cy + r + 30)]), fill=ARROW_A)
    ri = 9
    d.ellipse([(cx - ri) * S, (cy - ri) * S, (cx + ri) * S, (cy + ri) * S], fill=0)
    d.ellipse([(cx - 30) * S, (BY - 10) * S, (cx + 30) * S, (BY + 2) * S], outline=ROAD_A, width=3 * S)
    return im

# ------------------------------------------------------------------ вывод -----
def down(im):
    return im.resize((W, H), Image.LANCZOS)

def crop(im):
    bb = im.getbbox()
    return (im.crop(bb), bb[0], bb[1]) if bb else (im, 0, 0)

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

PERSP = 0           # перспектива (верх сужается, доля ширины с каждой стороны; 0.17 — как на приборке). 0 — выключена

def persp(im):
    """Проективное сжатие кверху: низ — полная ширина, верх — уже (дорога уходит вдаль)."""
    if not PERSP: return im
    import numpy as np
    w, h = im.size
    k = PERSP * w
    out = [(0, 0), (w, 0), (w, h), (0, h)]                    # углы результата
    src = [(-k * 0 + 0, 0), (w, 0), (w, h), (0, h)]
    # результат: верхний край сужен -> точки результата (k,0),(w-k,0) берутся из верхних углов источника
    dst = [(k, 0), (w - k, 0), (w, h), (0, h)]
    A = []
    for (x, y), (u, v) in zip(dst, src):
        A.append([x, y, 1, 0, 0, 0, -u * x, -u * y]); A.append([0, 0, 0, x, y, 1, -v * x, -v * y])
    B = [c for (u, v) in src for c in (u, v)]
    co = np.linalg.solve(np.array(A, float), np.array(B, float))
    return im.transform(im.size, Image.PERSPECTIVE, tuple(co), Image.BICUBIC)

def road_layer(union):
    """Слой дороги: серая заливка + белая кайма снаружи. union — холст (S-кратный), где дорога
    и след стрелки нарисованы одной яркостью."""
    q = 4
    u = union.resize((W * q, H * q), Image.LANCZOS)
    m = u.point(lambda v: 255 if v >= 40 else 0)
    dil = m.filter(ImageFilter.MaxFilter(2 * ROAD_BORDER * q + 1))
    border = ImageChops.subtract(dil, m)
    fill = m.point(lambda v: ROAD_FILL if v else 0)
    return ImageChops.lighter(border, fill).resize((W, H), Image.LANCZOS)

SW, SH = 38, 23                 # маленькая стрелка "следующий манёвр" (1/5)

def main():
    global ROAD_A
    global ARROW_A
    imgs = []    # (name, im, x, y)
    roads = {}   # name -> (im, x, y): слой дороги под большой стрелкой

    def build(prefix, size, round_road=None, road_layers=False):
        global ROAD_A, ARROW_A
        def add(name, fn, *args, road=True):
            global ROAD_A, ARROW_A
            saved = ROAD_A
            if road_layers:
                ROAD_A = 0                                   # слой стрелки — без дороги
            ARROW_A = 255
            big = fn(*args)
            if road_layers: big = persp(big)
            if road_layers and road:
                ROAD_A = ROAD_FILL; ARROW_A = ROAD_FILL      # слой дороги: дорога + след стрелки одним тоном
                un = persp(fn(*args))
                rc, rx, ry = crop(road_layer(un))
                roads[prefix + name] = (rc, rx, ry)
            ROAD_A = saved; ARROW_A = 255
            c, x, y = crop(big.resize(size, Image.LANCZOS))
            imgs.append((prefix + name, c, x, y))
        for s_ in range(16): add(f"turn_{s_:02d}", img_turn, s_)
        saved = ROAD_A
        if round_road is not None: ROAD_A = round_road    # у маленьких колец серое кольцо оставляем
        for s_ in range(16): add(f"round_{s_:02d}", img_roundabout, s_)
        ROAD_A = saved
        add("exit_l", img_exit, False); add("exit_r", img_exit, True)
        add("fork_l", img_fork, False); add("fork_r", img_fork, True)
        add("uturn_l", img_uturn, True); add("uturn_r", img_uturn, False)
        add("arrived", img_arrived, road=False)

    build("nav_", (W, H), road_layers=True)
    road_saved = ROAD_A
    ROAD_A = 0                      # у маленьких стрелок без серой дороги
    build("nav_s_", (SW, SH), round_road=110)
    ROAD_A = road_saved

    total = 0
    with open(os.path.join(OUT, "hud_nav_images.c"), "w") as f:
        f.write("/* Сгенерировано tools/gen_nav_images.py — не править вручную */\n")
        f.write('#define LV_LVGL_H_INCLUDE_SIMPLE\n#include "lvgl.h"\n#include "hud_nav_images.h"\n\n')
        for n, (im, x, y) in roads.items():
            n = "road_" + n
            data = pack4(im); total += len(data)
            rows = [", ".join(f"0x{b:02x}" for b in data[i:i + 24]) for i in range(0, len(data), 24)]
            f.write(f"static const uint8_t {n}_map[] = {{\n    " + ",\n    ".join(rows) + "\n};\n")
            f.write(f"static const lv_img_dsc_t img_{n} = {{\n    .header.cf = LV_IMG_CF_ALPHA_4BIT, .header.always_zero = 0, "
                    f".header.reserved = 0,\n    .header.w = {im.width}, .header.h = {im.height}, "
                    f".data_size = {len(data)}, .data = {n}_map,\n}};\n\n")
        for n, im, x, y in imgs:
            data = pack4(im); total += len(data)
            rows = [", ".join(f"0x{b:02x}" for b in data[i:i + 24]) for i in range(0, len(data), 24)]
            f.write(f"static const uint8_t {n}_map[] = {{\n    " + ",\n    ".join(rows) + "\n};\n")
            f.write(f"static const lv_img_dsc_t img_{n} = {{\n    .header.cf = LV_IMG_CF_ALPHA_4BIT, .header.always_zero = 0, "
                    f".header.reserved = 0,\n    .header.w = {im.width}, .header.h = {im.height}, "
                    f".data_size = {len(data)}, .data = {n}_map,\n}};\n\n")
        def tbl(name, keys):
            f.write(f"static const NavImg {name}[{len(keys)}] = {{\n")
            for k in keys:
                n, im, x, y = next(t for t in imgs if t[0] == k)
                r = roads.get(n)
                if r: f.write(f"    {{ &img_{n}, {x}, {y}, &img_road_{n}, {r[1]}, {r[2]} }},\n")
                else: f.write(f"    {{ &img_{n}, {x}, {y}, NULL, 0, 0 }},\n")
            f.write("};\n")
        for pre in ("nav_", "nav_s_"):
            tbl(pre + "turn", [f"{pre}turn_{s_:02d}" for s_ in range(16)])
            tbl(pre + "round", [f"{pre}round_{s_:02d}" for s_ in range(16)])
            tbl(pre + "exit", [pre + "exit_l", pre + "exit_r"])
            tbl(pre + "fork", [pre + "fork_l", pre + "fork_r"])
            tbl(pre + "uturn", [pre + "uturn_l", pre + "uturn_r"])
            tbl(pre + "arrived", [pre + "arrived"])
        f.write("\nconst NavSet nav_big   = { nav_turn,   nav_round,   nav_exit,   nav_fork,   nav_uturn,   nav_arrived   };\n")
        f.write("const NavSet nav_small = { nav_s_turn, nav_s_round, nav_s_exit, nav_s_fork, nav_s_uturn, nav_s_arrived };\n")
    with open(os.path.join(OUT, "hud_nav_images.h"), "w") as f:
        f.write("/* Сгенерировано tools/gen_nav_images.py — не править вручную */\n"
                "#ifndef HUD_NAV_IMAGES_H\n#define HUD_NAV_IMAGES_H\n#include \"lvgl.h\"\n\n"
                f"#define NAV_AREA_W {W}\n#define NAV_AREA_H {H}\n"
                f"#define NAV_SMALL_W {SW}\n#define NAV_SMALL_H {SH}\n\n"
                "typedef struct { const lv_img_dsc_t *img; int16_t x, y; const lv_img_dsc_t *road; int16_t rx, ry; } NavImg;   /* x,y — смещение в своей области; road — слой дороги под стрелкой (серый с белой каймой) или NULL */\n\n"
                "/* Набор стрелок: turn[16] — сектор 0..15 (22.5 град, против часовой от 'прямо'),\n"
                "   round[16] — кольцо, сектор съезда; exit/fork/uturn: [0] налево, [1] направо */\n"
                "typedef struct {\n"
                "    const NavImg *turn, *round, *exit, *fork, *uturn, *arrived;\n"
                "} NavSet;\n\n"
                "extern const NavSet nav_big;     /* основная стрелка, область NAV_AREA_W x NAV_AREA_H   */\n"
                "extern const NavSet nav_small;   /* следующий манёвр, область NAV_SMALL_W x NAV_SMALL_H */\n\n#endif\n")

    # превью: большие + маленькие
    big = [t for t in imgs if not t[0].startswith("nav_s_")]
    small = [t for t in imgs if t[0].startswith("nav_s_")]
    cols = 8
    rows_b = (len(big) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (W + 6), rows_b * (H + 6) + 5 * (SH * 3 + 6)), (45, 45, 45))
    for i, (n, im, x, y) in enumerate(big):
        tile = Image.new("RGB", (W, H), (0, 0, 0))
        r = roads.get(n)
        if r: tile.paste(Image.new("RGB", r[0].size, (255, 255, 255)), (r[1], r[2]), r[0])
        tile.paste(Image.new("RGB", im.size, (48, 176, 240)), (x, y), im)
        sheet.paste(tile, ((i % cols) * (W + 6), (i // cols) * (H + 6)))
    y0 = rows_b * (H + 6)
    cs = (cols * (W + 6)) // (SW * 3 + 6)
    for i, (n, im, x, y) in enumerate(small):
        tile = Image.new("RGB", (SW, SH), (0, 0, 0))
        tile.paste(Image.new("RGB", im.size, (170, 170, 170)), (x, y), im)
        tile = tile.resize((SW * 3, SH * 3), Image.NEAREST)
        sheet.paste(tile, ((i % cs) * (SW * 3 + 6), y0 + (i // cs) * (SH * 3 + 6)))
    sheet.save(os.path.join(TOOLS, "preview_nav.png"))
    print(f"стрелок {len(imgs)}, данных {total} байт")

if __name__ == "__main__":
    main()
