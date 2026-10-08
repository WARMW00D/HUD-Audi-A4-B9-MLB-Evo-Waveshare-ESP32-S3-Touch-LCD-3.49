#!/usr/bin/env python3
"""
make_fonts.py — шрифты HUD: Montserrat Bold и Roboto Condensed Bold (обе SIL OFL 1.1)
------------------------------------------------------------------------------
1) Montserrat-Bold.ttf -> Montserrat-Bold-tnum.ttf: цифры переключаются на
   табличные (фича OpenType 'tnum'), все цифры одной ширины — число скорости
   не "прыгает" при смене цифр. lv_font_conv фичи OpenType не применяет,
   поэтому подмена делается прямо в cmap.
2) Генерация LVGL-шрифтов через lv_font_conv (нужен Node.js:
   npm i -g lv_font_conv) + правка LV_LVGL_H_INCLUDE_SIMPLE.

Выход: waveshare_hud_mockup/hud_font_*.c
Запуск из корня репозитория:  python tools/fonts/make_fonts.py
Нужен fontTools:         pip install fonttools
"""
import os, re, subprocess
from fontTools.ttLib import TTFont

HERE = os.path.dirname(os.path.abspath(__file__))
SKETCH = os.path.abspath(os.path.join(HERE, "..", "..", "waveshare_hud_mockup"))
SRC = os.path.join(HERE, "Montserrat-Bold.ttf")
TNUM = os.path.join(HERE, "Montserrat-Bold-tnum.ttf")
COND = os.path.join(HERE, "RobotoCondensed-Bold.ttf")   # цифры знака ограничения (узкие, как на дорожных знаках)

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

def gen(name, size, symbols, font):
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
    make_tnum()
    for n, sz, sym, font in FONTS:
        gen(n, sz, sym, font)
