#!/usr/bin/env python3
"""
gen_hud_images.py — генератор графики HUD (маски ALPHA_8BIT для LVGL 8)
------------------------------------------------------------------------------
Все картинки — одноцветные маски прозрачности (LV_IMG_CF_ALPHA_8BIT). Цвет
задаётся в прошивке стилем img_recolor, поэтому одна маска даёт и зелёную,
и красную мигалку, и серый/красный элемент машины.

Выход: waveshare_hud_mockup/hud_images.c / hud_images.h + tools/preview_*.png.
Запуск из корня репозитория: python3 tools/gen_hud_images.py   (нужен Pillow)
"""
import math, os
from PIL import Image, ImageDraw, ImageFilter, ImageChops

S = 8                      # суперсэмплинг
TOOLS = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(TOOLS, "..", "waveshare_hud_mockup")      # папка скетча

def canvas(w, h):
    return Image.new("L", (w * S, h * S), 0)

def down(img, w, h):
    return img.resize((w, h), Image.LANCZOS)

def P(pts):
    return [(x * S, y * S) for x, y in pts]

# ---------------------------------------------------------------- мигалки ----
BW = BH = 46

def arrow_shape(d, grow=0.0):
    """Стрелка влево в стиле контрольной лампы: треугольная голова + хвост."""
    g = grow
    cy = BH / 2
    tip, neck, tail = 4 - g, 22, 42 + g
    head_h, shaft_h = 19 + g, 7.5 + g
    pts = [(tip, cy), (neck + g * 0.4, cy - head_h), (neck + g * 0.4, cy - shaft_h),
           (tail, cy - shaft_h), (tail, cy + shaft_h), (neck + g * 0.4, cy + shaft_h),
           (neck + g * 0.4, cy + head_h)]
    d.polygon(P(pts), fill=255)

def blinker_masks():
    fill = canvas(BW, BH); arrow_shape(ImageDraw.Draw(fill))
    outl = canvas(BW, BH); arrow_shape(ImageDraw.Draw(outl), grow=2.6)
    # мягкий блик сверху на заливке (яркость через альфу: 1.0 -> 0.82 к низу)
    grad = Image.linear_gradient("L").resize((BW * S, BH * S)).point(lambda v: 255 - int(v * 0.18))
    fill = Image.composite(grad, Image.new("L", fill.size, 0), fill)
    return down(outl, BW, BH), down(fill, BW, BH)

# ---------------------------------------------------------------- машина -----
# Вид сверху в стиле приборки: светлый кузов с тенями, тёмные стёкла,
# красная полоса фонарей. Яркость = альфа маски (фон HUD чёрный, поэтому
# белая маска с альфой рисует оттенки серого). Каждый открываемый элемент —
# две маски: чёрная подложка (закрывает кузов/проём) + красная с тенями.
CW, CH = 192, 140
CX = CW / 2
TOP, BOT = 6, 134
HW = 29                      # полуширина кузова

def rrect(d, box, r, **kw):
    d.rounded_rectangle([c * S for c in box], radius=r * S, **kw)

def half_width(y):
    """Полуширина кузова на высоте y: закруглённый нос, прямые борта, скруглённая корма."""
    nose_r, tail_r = 20.0, 11.0
    if y < TOP + nose_r:                       # нос: эллиптическое скругление
        t = (TOP + nose_r - y) / nose_r
        return (HW - 2) * math.sqrt(max(0.0, 1 - t ** 2.4))
    if y > BOT - tail_r:                       # корма
        t = (y - (BOT - tail_r)) / tail_r
        return HW * math.sqrt(max(0.0, 1 - t ** 2.6))
    # лёгкое сужение к носу (капот уже салона на 2 px)
    k = min(1.0, (y - TOP - nose_r) / 25.0)
    return HW - 2 + 2 * k

def body_shape(inset=0.0):
    m = canvas(CW, CH); d = ImageDraw.Draw(m)
    ys = [TOP + inset + (BOT - TOP - 2 * inset) * i / 400 for i in range(401)]
    left = [(CX - max(0.0, half_width(y) - inset), y) for y in ys]
    right = [(CX + max(0.0, half_width(y) - inset), y) for y in reversed(ys)]
    d.polygon(P(left + right), fill=255)
    return m

def vgrad(y0, y1, a0, a1):
    """Вертикальный градиент альфы на весь холст, между y0 и y1."""
    g = Image.new("L", (CW * S, CH * S), 0); px = ImageDraw.Draw(g)
    for y in range(CH * S):
        t = min(max((y / S - y0) / max(y1 - y0, 1e-6), 0), 1)
        px.line([(0, y), (CW * S, y)], fill=int(a0 + (a1 - a0) * t))
    return g

def paint(base, mask, value_img):
    return Image.composite(value_img, base, mask)

def region(y0, y1, inset=2.5):
    cut = canvas(CW, CH); ImageDraw.Draw(cut).rectangle([0, y0 * S, CW * S, y1 * S], fill=255)
    return ImageChops.multiply(body_shape(inset), cut)

def body_mask():
    l, r = CX - HW, CX + HW
    img = Image.composite(Image.new("L", (CW * S, CH * S), 70), canvas(CW, CH), body_shape())
    img = paint(img, region(TOP, 42), vgrad(TOP, 42, 235, 185))           # капот
    ws = canvas(CW, CH); ImageDraw.Draw(ws).polygon(P([(l + 5, 42), (r - 5, 42), (r - 9, 56), (l + 9, 56)]), fill=255)
    img = paint(img, ws, vgrad(42, 56, 60, 35))                          # лобовое
    roof = canvas(CW, CH); rrect(ImageDraw.Draw(roof), (l + 9, 56, r - 9, 100), 4, fill=255)
    img = paint(img, roof, vgrad(56, 100, 215, 175))                     # крыша
    rw = canvas(CW, CH); ImageDraw.Draw(rw).polygon(P([(l + 9, 100), (r - 9, 100), (r - 6, 112), (l + 6, 112)]), fill=255)
    img = paint(img, rw, vgrad(100, 112, 35, 55))                        # заднее стекло
    img = paint(img, region(112, 124, 3), vgrad(112, 124, 150, 120))     # крышка багажника
    img = paint(img, region(124, BOT, 2), Image.new("L", img.size, 85))  # бампер
    d = ImageDraw.Draw(img)
    for side in (-1, 1):                                                  # боковые стёкла
        x0 = CX + side * (HW - 4); x1 = CX + side * (HW - 8.5)
        d.polygon(P([(x0, 50), (x1, 57), (x1, 99), (x0, 104)]), fill=40)
        for y in (48, 76, 103):                                           # швы дверей
            d.line(P([(CX + side * HW, y), (CX + side * (HW - 4), y)]), fill=20, width=S)
    # контур (светлый кант)
    edge = ImageChops.subtract(body_shape(), body_shape(1.4))
    img = ImageChops.lighter(img, edge.point(lambda v: int(v * 0.95)))
    # зеркала
    for side in (-1, 1):
        x = CX + side * HW
        d.polygon(P([(x, 44), (x + side * 7, 45.5), (x + side * 7.5, 49), (x, 50)]), fill=200)
    return down(img, CW, CH)

def lights_mask():
    m = canvas(CW, CH); d = ImageDraw.Draw(m)
    for side in (-1, 1):
        d.line(P([(CX + side * (HW - 2), 122), (CX + side * 8, 124.5)]), fill=230, width=int(2.2 * S))
    return down(m, CW, CH)

def door_quad(side, y0, length, angle_deg, thick):
    hx = CX + (-HW if side == "L" else HW)
    sgn = -1 if side == "L" else 1
    a = math.radians(angle_deg)
    dx, dy = sgn * math.sin(a), math.cos(a)
    nx, ny = -sgn * math.cos(a), math.sin(a)
    p0 = (hx + nx * 1.0, y0 + ny * 1.0)
    p1 = (p0[0] + dx * length, p0[1] + dy * length)
    return [p0, p1, (p1[0] + nx * thick, p1[1] + ny * thick), (p0[0] + nx * thick, p0[1] + ny * thick)], (dx, dy), (nx, ny)

def elem_masks(kind):
    """-> (подложка, красная маска)"""
    under = canvas(CW, CH); red = canvas(CW, CH)
    du, dr = ImageDraw.Draw(under), ImageDraw.Draw(red)
    if kind in ("FL", "FR", "RL", "RR"):
        side = kind[1]
        y0, ln = (47, 30) if kind[0] == "F" else (77, 26)
        q, (dx, dy), (nx, ny) = door_quad(side, y0, ln, 27, 11)
        # проём в борту — тёмный
        x = CX + (-HW if side == "L" else HW - 3.5)
        du.rectangle([x * S, (y0 + 1) * S, (x + 3.5) * S, (y0 + ln - 1) * S], fill=255)
        du.polygon(P(q), fill=255)
        # дверь: внешняя обшивка (светлее) + торец/внутренняя сторона (темнее)
        outer = [q[0], q[1], ((q[1][0] + q[2][0]) / 2, (q[1][1] + q[2][1]) / 2), ((q[0][0] + q[3][0]) / 2, (q[0][1] + q[3][1]) / 2)]
        dr.polygon(P(q), fill=150)
        dr.polygon(P(outer), fill=255)
        # ручка-вырез
        c = (q[0][0] + dx * ln * 0.55 + nx * 4.6, q[0][1] + dy * ln * 0.55 + ny * 4.6)
        dr.line(P([c, (c[0] + dx * 7, c[1] + dy * 7)]), fill=90, width=int(1.4 * S))
    elif kind == "HOOD":
        reg = region(TOP, 42, 1.5)
        under = reg
        red = Image.composite(vgrad(TOP, 42, 255, 190), canvas(CW, CH), reg)
    elif kind == "TRUNK":
        reg = region(100, BOT - 2, 1.5)
        under = reg
        red = Image.composite(vgrad(100, BOT, 205, 255), canvas(CW, CH), reg)
        dr = ImageDraw.Draw(red)
        dr.line(P([(CX - HW + 6, 112), (CX + HW - 6, 112)]), fill=130, width=S)
    return down(under, CW, CH), down(red, CW, CH)

# ---------------------------------------------------------------- значки -----
AW, AH = 64, 54              # значок ACC / Lane Assist (место прежнего "ACC/LKA")

def car_outline(d, cx, top, w, h, t, erase=False):
    """Машина сзади контуром (стиль контрольных ламп): кабина-трапеция, пояс с
    зеркалами, кузов, фонари, бампер, колёса. erase=True — залить силуэт нулём
    (чтобы передняя машина закрывала задние)."""
    cab_b = top + h * 0.40
    body_b = top + h * 0.82
    if erase:
        sil = [(cx - w * 0.24, top), (cx + w * 0.24, top), (cx + w * 0.34, cab_b),
               (cx + w * 0.50, cab_b), (cx + w * 0.50, top + h), (cx - w * 0.50, top + h),
               (cx - w * 0.50, cab_b), (cx - w * 0.34, cab_b)]
        d.polygon(P(sil), fill=0)
        return
    lw = int(t * S)
    # кабина
    d.line(P([(cx - w * 0.33, cab_b), (cx - w * 0.23, top + t / 2), (cx + w * 0.23, top + t / 2), (cx + w * 0.33, cab_b)]),
           fill=255, width=lw, joint="curve")
    # пояс с зеркалами
    d.line(P([(cx - w * 0.56, cab_b - t * 0.2), (cx + w * 0.56, cab_b - t * 0.2)]), fill=255, width=lw)
    # кузов
    rrect(d, (cx - w * 0.47, cab_b, cx + w * 0.47, body_b), h * 0.09, outline=255, width=lw)
    # фонари
    ly = top + h * 0.58
    for sx in (-1, 1):
        d.line(P([(cx + sx * w * 0.40, ly), (cx + sx * w * 0.20, ly)]), fill=255, width=int(t * 1.25 * S))
    # бампер
    d.line(P([(cx - w * 0.14, top + h * 0.71), (cx + w * 0.14, top + h * 0.71)]), fill=255, width=int(t * 0.9 * S))
    # колёса
    for sx in (-1, 1):
        x0, x1 = sorted((cx + sx * w * 0.45, cx + sx * w * 0.27))
        rrect(d, (x0, body_b - t / 2, x1, top + h), 0.8, fill=255)

def assist_masks():
    out = {}
    m = canvas(AW, AH); car_outline(ImageDraw.Draw(m), AW / 2, 1.5, 23, 15, 1.7); out["front"] = down(m, AW, AH)
    # волны: верхняя шире, все выпуклые вверх
    m = canvas(AW, AH); d = ImageDraw.Draw(m)
    for y, hw in ((18.5, 13.5), (22.2, 10.5), (25.9, 7.5)):
        R = 26.0
        a = math.degrees(math.asin(hw / R))
        d.arc([(AW / 2 - R) * S, (y) * S, (AW / 2 + R) * S, (y + 2 * R) * S], 270 - a, 270 + a, fill=255, width=int(1.9 * S))
    out["arcs"] = down(m, AW, AH)
    m = canvas(AW, AH); car_outline(ImageDraw.Draw(m), AW / 2, 29.5, 27, 23.5, 1.9); out["own"] = down(m, AW, AH)
    # линия полосы (толстая, в перспективе) + треугольник снаружи, указывающий внутрь
    m = canvas(AW, AH); d = ImageDraw.Draw(m)
    d.polygon(P([(4.5, 53.5), (8.8, 53.5), (17.2, 28), (14.6, 28)]), fill=255)
    ty = 41
    d.polygon(P([(0.5, ty - 3.2), (0.5, ty + 3.2), (5.6, ty)]), fill=255)
    out["line_l"] = down(m, AW, AH)
    out["line_r"] = out["line_l"].transpose(Image.FLIP_LEFT_RIGHT)
    return out

def jam_mask():
    """Режим пробки: три машины уступом, передняя закрывает задние."""
    W, H = 40, 22
    m = canvas(W, H); d = ImageDraw.Draw(m)
    for n, (cx, top) in enumerate(((11, 1), (18, 3.5), (26, 6))):
        if n: car_outline(d, cx, top, 20, 15.5, 1.6, erase=True)
        car_outline(d, cx, top, 20, 15.5, 1.6)
    return down(m, W, H)

def presense_tri():
    """Предупреждающий треугольник со скруглёнными углами и вырезанным '!'.
    Скругление: внутренний треугольник + обводка толщиной 2r (сумма Минковского)."""
    W, H = 92, 80
    r = 5.0
    m = canvas(W, H); d = ImageDraw.Draw(m)
    A, B, C = (W / 2, 1.5 + r * 1.9), (W - 1.5 - r * 1.1, H - 1.5 - r), (1.5 + r * 1.1, H - 1.5 - r)
    d.polygon(P([A, B, C]), fill=255)
    d.line(P([A, B, C, A]), fill=255, width=int(2 * r * S), joint="curve")
    for x, y in (A, B, C):
        d.ellipse([(x - r) * S, (y - r) * S, (x + r) * S, (y + r) * S], fill=255)
    cx = W / 2
    d.polygon(P([(cx - 5.2, 24), (cx + 5.2, 24), (cx + 3.4, 54), (cx - 3.4, 54)]), fill=0)
    d.ellipse([(cx - 5) * S, 59 * S, (cx + 5) * S, 69 * S], fill=0)
    return down(m, W, H)

def text_mask(text, px):
    from PIL import ImageFont
    fp = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fonts", "Montserrat-Bold.ttf")
    f = ImageFont.truetype(fp, px * S)
    im = Image.new("L", (len(text) * px * S, px * 2 * S), 0)
    ImageDraw.Draw(im).text((2 * S, 2 * S), text, font=f, fill=255)
    im = im.crop(im.getbbox())
    return im.resize((round(im.width / S), round(im.height / S)), Image.LANCZOS)

def sign_car():
    """Машина сзади, залитая — для знака «обгон запрещён» (3.20)."""
    W, H = 18, 15
    m = canvas(W, H); d = ImageDraw.Draw(m)
    cx = W / 2
    d.polygon(P([(cx - 5.5, 6.5), (cx - 4, 1), (cx + 4, 1), (cx + 5.5, 6.5)]), fill=255)       # кабина
    d.polygon(P([(cx - 4.0, 5.8), (cx - 3, 2.2), (cx + 3, 2.2), (cx + 4.0, 5.8)]), fill=0)     # стекло
    rrect(d, (0.5, 6, W - 0.5, 12.5), 1.5, fill=255)                                          # кузов
    for sx in (-1, 1):
        x = cx + sx * 6
        d.rectangle([(x - 1.6) * S, 8 * S, (x + 1.6) * S, 9.6 * S], fill=0)                   # фонари
        rrect(d, (x - 1.8, 11.5, x + 1.8, 14.8), 0.6, fill=255)                                # колёса
    return down(m, W, H)

def fuel_icon():
    """Маленькая колонка АЗС для «сколько заправить» (свой рисунок, ISO-подобный)."""
    W, H = 15, 16
    m = canvas(W, H); d = ImageDraw.Draw(m)
    rrect(d, (0.8, 0.8, 9.2, 13.6), 1.2, fill=255)                 # корпус
    rrect(d, (2.4, 2.6, 7.6, 6.4), 0.6, fill=0)                    # окошко
    rrect(d, (0.0, 13.0, 10.0, 15.6), 0.8, fill=255)               # основание
    w = int(1.9 * S)
    d.line(P([(9.0, 6.8), (11.4, 6.8), (11.4, 12.4)]), fill=255, width=w, joint="curve")       # шланг вниз
    d.arc([10.5 * S, 10.6 * S, 14.4 * S, 14.6 * S], 0, 180, fill=255, width=w)                  # петля
    d.line(P([(13.9, 12.6), (13.9, 4.4), (11.4, 1.8)]), fill=255, width=w, joint="curve")      # пистолет
    return down(m, W, H)

def lim_mask():
    """Лимитер: надпись LIM (Montserrat Bold, OFL)."""
    from PIL import ImageFont
    fp = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fonts", "Montserrat-Bold.ttf")
    f = ImageFont.truetype(fp, 18 * S)
    im = Image.new("L", (60 * S, 30 * S), 0)
    ImageDraw.Draw(im).text((2 * S, 2 * S), "LIM", font=f, fill=255)
    im = im.crop(im.getbbox())
    return im.resize((round(im.width / S), round(im.height / S)), Image.LANCZOS)

def mask_from_file(path, invert, h_target, w_target=None, margin=0):
    """Значок из картинки: яркость (или инверсия) -> альфа, обрезка, масштаб.
    margin — сколько пикселей по краю исходника стереть (рамки/уголки)."""
    im = Image.open(path).convert("L")
    if invert: im = ImageChops.invert(im)
    im = im.point(lambda v: 0 if v < 70 else min(255, int((v - 70) * 255 / 150)))
    if margin:
        clean = Image.new("L", im.size, 0)
        clean.paste(im.crop((margin, margin, im.width - margin, im.height - margin)), (margin, margin))
        im = clean
    im = im.crop(im.getbbox())
    if w_target is None: w_target = max(1, round(im.width * h_target / im.height))
    return im.resize((w_target, h_target), Image.LANCZOS)

# ---------------------------------------------------------------- вывод ------
def crop(img):
    bb = img.getbbox()
    if not bb: return img, 0, 0
    return img.crop(bb), bb[0], bb[1]

def c_array(name, img):
    w, h = img.size
    data = list(img.tobytes())
    rows = []
    for i in range(0, len(data), 24):
        rows.append("    " + ",".join(f"0x{v:02x}" for v in data[i:i + 24]) + ",")
    return (f"static const uint8_t {name}_map[] = {{\n" + "\n".join(rows) + "\n};\n"
            f"const lv_img_dsc_t {name} = {{\n"
            f"    .header.cf = LV_IMG_CF_ALPHA_8BIT, .header.always_zero = 0, .header.reserved = 0,\n"
            f"    .header.w = {w}, .header.h = {h}, .data_size = {w * h}, .data = {name}_map,\n}};\n\n")

ICON_SRC = os.environ.get("HUD_ICON_DIR", os.path.join(os.path.dirname(os.path.abspath(__file__)), "icons"))

def main():
    items, offs = [], []
    outl, fill = blinker_masks()
    items += [("img_blink_l_outline", outl), ("img_blink_l_fill", fill),
              ("img_blink_r_outline", outl.transpose(Image.FLIP_LEFT_RIGHT)),
              ("img_blink_r_fill", fill.transpose(Image.FLIP_LEFT_RIGHT))]

    def add(name, im, off_name=None):
        c, x, y = crop(im)
        items.append((name, c))
        if off_name: offs.append((off_name, x, y))
        return c, x, y

    body = add("img_car_body", body_mask(), "CAR_BODY")
    add("img_car_lights", lights_mask(), "CAR_LIGHTS")
    parts = []
    for k in ("FL", "FR", "RL", "RR", "HOOD", "TRUNK"):
        u, r = elem_masks(k)
        cu = add(f"img_car_{k.lower()}_under", u, f"CAR_{k}_UNDER")
        cr = add(f"img_car_{k.lower()}", r, f"CAR_{k}")
        parts.append((cu, cr))

    am = assist_masks()
    assist = {}
    for k in ("own", "front", "arcs", "line_l", "line_r"):
        assist[k] = add(f"img_assist_{k}", am[k], f"ASSIST_{k.upper()}")

    acc_icon = mask_from_file(os.path.join(ICON_SRC, "acc_set.png"), True, 26, margin=12)
    items.append(("img_acc_set", acc_icon))
    jam_icon = jam_mask()
    items.append(("img_traffic_jam", jam_icon))
    lim_icon = lim_mask()
    items.append(("img_limiter", lim_icon))
    items.append(("img_presense_tri", presense_tri()))
    items.append(("img_sign_car", sign_car()))
    items.append(("img_fuel_icon", fuel_icon()))
    items.append(("img_presense_txt", text_mask("PreSense", 22)))

    with open(os.path.join(OUT, "hud_images.c"), "w") as f:
        f.write("/* Сгенерировано tools/gen_hud_images.py — не править вручную */\n")
        f.write("#define LV_LVGL_H_INCLUDE_SIMPLE\n#include \"lvgl.h\"\n\n")
        for n, im in items: f.write(c_array(n, im))
    with open(os.path.join(OUT, "hud_images.h"), "w") as f:
        f.write("/* Сгенерировано tools/gen_hud_images.py — не править вручную */\n"
                "#ifndef HUD_IMAGES_H\n#define HUD_IMAGES_H\n#include \"lvgl.h\"\n\n")
        for n, _ in items: f.write(f"LV_IMG_DECLARE({n});\n")
        f.write(f"\n/* Смещения: CAR_* внутри области {CW}x{CH}, ASSIST_* внутри {AW}x{AH} */\n")
        for n, x, y in offs: f.write(f"#define {n}_X {x}\n#define {n}_Y {y}\n")
        f.write("\n#endif\n")

    # ---- превью машины: все элементы открыты + по одному
    def render_car(open_set):
        pv = Image.new("RGB", (CW, CH), (0, 0, 0))
        b, bx, by = body
        pv.paste(Image.new("RGB", b.size, (225, 225, 225)), (bx, by), b)
        lt = items[[n for n, _ in items].index("img_car_lights")][1]
        lx, ly = [(x, y) for n, x, y in offs if n == "CAR_LIGHTS"][0]
        pv.paste(Image.new("RGB", lt.size, (200, 20, 20)), (lx, ly), lt)
        for k, ((u, ux, uy), (r, rx, ry)) in zip(("FL", "FR", "RL", "RR", "HOOD", "TRUNK"), parts):
            if k in open_set:
                pv.paste(Image.new("RGB", u.size, (0, 0, 0)), (ux, uy), u)
                pv.paste(Image.new("RGB", r.size, (235, 25, 25)), (rx, ry), r)
        return pv
    sets = [set(), {"FL"}, {"FR"}, {"RL"}, {"TRUNK"}, {"HOOD"}, {"FL", "FR", "RL", "RR", "HOOD", "TRUNK"}]
    sheet = Image.new("RGB", (CW * 4 + 30, CH * 2 + 10), (40, 40, 40))
    for n, st in enumerate(sets):
        sheet.paste(render_car(st), ((n % 4) * (CW + 10), (n // 4) * (CH + 10)))
    sheet.resize((sheet.width * 2, sheet.height * 2), Image.LANCZOS).save(os.path.join(TOOLS, "preview_car.png"))

    # ---- превью значков: ACC активен + LKA обе линии / LKA одна жёлтая / ACC пассив / пробка / ACC-скорость
    G, Y, W, GR = (40, 230, 40), (255, 200, 0), (235, 235, 235), (150, 150, 150)
    def render_assist(cols):
        pv = Image.new("RGB", (AW, AH), (0, 0, 0))
        for k, col in cols.items():
            if col is None: continue
            im, x, y = assist[k]
            pv.paste(Image.new("RGB", im.size, col), (x, y), im)
        return pv
    variants = [
        {"own": G, "front": G, "arcs": G, "line_l": G, "line_r": G},
        {"own": G, "front": G, "arcs": G, "line_l": Y, "line_r": G},
        {"own": W, "front": W, "arcs": None, "line_l": None, "line_r": None},
        {"own": G, "front": None, "arcs": None, "line_l": G, "line_r": G},
    ]
    sh = Image.new("RGB", (AW * 4 + 40 + 110, AH + 10), (40, 40, 40))
    for n, v in enumerate(variants):
        sh.paste(render_assist(v), (5 + n * (AW + 8), 5))
    x0 = 5 + 4 * (AW + 8)
    for im, x in ((acc_icon, x0), (jam_icon, x0 + 32), (lim_icon, x0 + 70)):
        tile = Image.new("RGB", im.size, (0, 0, 0)); tile.paste(Image.new("RGB", im.size, G), (0, 0), im)
        sh.paste(tile, (x, 20))
    sh.resize((sh.width * 4, sh.height * 4), Image.NEAREST).save(os.path.join(TOOLS, "preview_icons.png"))

    total = sum(im.size[0] * im.size[1] for _, im in items)
    print(f"картинок {len(items)}, данных {total} байт")

if __name__ == "__main__":
    main()
