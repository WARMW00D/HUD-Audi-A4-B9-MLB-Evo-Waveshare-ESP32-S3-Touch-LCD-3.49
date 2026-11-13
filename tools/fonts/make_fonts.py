#!/usr/bin/env python3
"""
make_fonts.py — шрифты HUD: Montserrat Bold, Roboto Condensed Bold, Overpass Medium и Inter (все SIL OFL 1.1)
------------------------------------------------------------------------------
1) Montserrat-Bold.ttf -> Montserrat-Bold-tnum.ttf: цифры переключаются на
   табличные (фича OpenType 'tnum'), все цифры одной ширины — число скорости
   не "прыгает" при смене цифр. lv_font_conv фичи OpenType не применяет,
   поэтому подмена делается прямо в cmap.
2) Минималистичный режим: Overpass Medium (цифры скорости/расстояния) сжимается по ширине в 0.90 прямо в
   контурах (LVGL масштабирует глифы только равномерно) -> Overpass-Medium-sq90.ttf; размер шрифта
   подбирается по высоте цифры в пикселях ("dh"). Inter Regular/Bold (подмножество) — единицы и расстояние до камеры.
3) Генерация LVGL-шрифтов через lv_font_conv (нужен Node.js:
   npm i -g lv_font_conv) + правка LV_LVGL_H_INCLUDE_SIMPLE.

Выход: waveshare_hud_mockup/hud_font_*.c
Запуск из корня репозитория:  python tools/fonts/make_fonts.py [имя_шрифта ...]
Нужен fontTools:         pip install fonttools
"""
import os, re, subprocess
from fontTools.ttLib import TTFont

HERE = os.path.dirname(os.path.abspath(__file__))
SKETCH = os.path.abspath(os.path.join(HERE, "..", "..", "waveshare_hud_mockup"))
SRC = os.path.join(HERE, "Montserrat-Bold.ttf")
TNUM = os.path.join(HERE, "Montserrat-Bold-tnum.ttf")
COND = os.path.join(HERE, "RobotoCondensed-Bold.ttf")   # цифры знака ограничения (узкие, как на дорожных знаках)
OVER = os.path.join(HERE, "Overpass-Medium.ttf")        # цифры минималистичного режима (подмножество: 0-9 . ,)
OVER_SQ = os.path.join(HERE, "Overpass-Medium-sq90.ttf")
SQUEEZE = 0.90                                          # сжатие цифр по ширине
INTER_R = os.path.join(HERE, "Inter-Regular-sub.otf")   # единицы (км/ч, м, km, mi, ft), подмножество
INTER_B = os.path.join(HERE, "Inter-Bold-sub.otf")      # расстояние до камеры

# имя файла/символа, --size, символы. Размеры подобраны под прежнюю высоту цифр.
FONTS = [
    ("hud_font_speed", 116, "0123456789", TNUM),
    ("hud_font_gear",   22, "PRNDSMEOfroad123456789", TNUM),
    ("hud_font_fuel",   22, "0123456789.Lлgal ", TNUM),      # сколько заправить: "13 л" / "3.4 gal"
    ("hud_font_route",  28, "0123456789:.kmhift кмч", TNUM),   # без букв с выносными вниз (y): иначе растёт межстрочный
    ("hud_font_small",  26, "0123456789", TNUM),
    ("hud_font_kmh",    38, "km/hpкмч", TNUM),
    # знак: круг 64 px, красное кольцо 7 px, белое поле 50 px. Размеры — максимальные,
    # при которых "88" / "188" целиком помещаются в белое поле (диагональ текста <= 48 px)
    ("hud_font_sign2",  40, "0123456789", COND),
    ("hud_font_sign3",  30, "0123456789", COND),
    # меню настроек: латиница + кириллица (встроенные шрифты LVGL кириллицы не имеют)
    ("hud_font_menu",   20, "range:0x20-0x7E,0x401,0x410-0x44F,0x451", SRC),
    # минималистичный режим: высота цифр в пикселях (dh) -> размер шрифта подбирается по контуру "0"
    ("hud_font_mspeed", ("dh", 88), "0123456789", OVER_SQ),   # скорость
    ("hud_font_mdist",  ("dh", 34), "0123456789.,", OVER_SQ), # расстояние до манёвра, заданная скорость ACC
    ("hud_font_munit22", 22, "кмч/khpim ", INTER_R),         # «км/ч» рядом со скоростью
    ("hud_font_munit20", 20, "кмч/khpimft ", INTER_R),       # единицы у расстояния и заданной скорости
    ("hud_font_mcam",   24, "0123456789мmft", INTER_B),      # расстояние до камеры
    # цифры знаков минималистичного режима: размер — максимальный, при котором самая широкая надпись ("10" / "100")
    # целиком помещается в белое поле круга (диагональ чернил <= диаметр поля - 2 px): знак 56 px с кольцом 6 -> поле 44;
    # круг на жёлтом квадрате камеры 44 px с кольцом 5 -> поле 34
    ("hud_font_msign2", 33, "0123456789", COND),
    ("hud_font_msign3", 24, "0123456789", COND),
    ("hud_font_mcdisc2", 25, "0123456789", COND),
    ("hud_font_mcdisc3", 19, "0123456789", COND),
]

def make_tnum():
    f = TTFont(SRC)
    gsub = f["GSUB"].table
    rec = [fr for fr in gsub.FeatureList.FeatureRecord if fr.FeatureTag == "tnum"][0]
    m = {}
    for li in rec.Feature.LookupListIndex:
        for st in gsub.LookupList.Lookup[li].SubTable:
            if hasattr(st, "mapping"):
                m.update(st.mapping)
    for t in f["cmap"].tables:
        for c in list(t.cmap):
            if chr(c) in "0123456789" and t.cmap[c] in m:
                t.cmap[c] = m[t.cmap[c]]
    f.save(TNUM)

def make_squeezed():
    """Overpass-Medium.ttf -> Overpass-Medium-sq90.ttf: контуры сжаты по X в SQUEEZE раз"""
    from fontTools.pens.transformPen import TransformPen
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    f = TTFont(OVER)
    gs = f.getGlyphSet()
    glyf, hmtx = f["glyf"], f["hmtx"]
    for name in f.getGlyphOrder():
        pen = TTGlyphPen(gs)
        gs[name].draw(TransformPen(pen, (SQUEEZE, 0, 0, 1, 0, 0)))
        g = pen.glyph()
        adv, _ = hmtx[name]
        glyf[name] = g
        g.recalcBounds(glyf)
        hmtx[name] = (round(adv * SQUEEZE), getattr(g, "xMin", 0))
    if "hdmx" in f: del f["hdmx"]
    if "LTSH" in f: del f["LTSH"]
    f.save(OVER_SQ)

def digit_size(font, dh):
    """размер шрифта (px на em), при котором контур «0» имеет высоту dh px"""
    f = TTFont(font)
    g = f["glyf"][f.getBestCmap()[ord("0")]]
    g.recalcBounds(f["glyf"])
    return round(dh * f["head"].unitsPerEm / (g.yMax - g.yMin))

def gen(name, size, symbols, font):
    if isinstance(size, tuple): size = digit_size(font, size[1])
    out = os.path.join(SKETCH, name + ".c")
    chars = ["--range", symbols[6:]] if symbols.startswith("range:") else ["--symbols", symbols]
    subprocess.run(["lv_font_conv", "--font", font, "--size", str(size), "--bpp", "4",
                    "--format", "lvgl"] + chars + ["-o", out,
                    "--force-fast-kern-format", "--no-prefilter"], check=True, shell=(os.name == "nt"))
    s = open(out, encoding="utf-8").read()
    s = s.replace("#ifdef LV_LVGL_H_INCLUDE_SIMPLE",
                  "#define LV_LVGL_H_INCLUDE_SIMPLE\n#ifdef LV_LVGL_H_INCLUDE_SIMPLE", 1)
    s = re.sub(r"--font \S*[/\\]([^/\\\s]+\.ttf)", r"--font \1", s)
    s = re.sub(r"-o \S*" + name + r"\.c", "-o " + name + ".c", s)
    open(out, "w", encoding="utf-8").write(s)
    print("ok", name)

if __name__ == "__main__":
    import sys
    only = sys.argv[1:]                       # python tools/fonts/make_fonts.py [имя_шрифта ...] — только указанные
    todo = [f for f in FONTS if not only or f[0] in only]
    if any(f[3] == TNUM for f in todo): make_tnum()
    if any(f[3] == OVER_SQ for f in todo): make_squeezed()
    for n, sz, sym, font in todo:
        gen(n, sz, sym, font)
