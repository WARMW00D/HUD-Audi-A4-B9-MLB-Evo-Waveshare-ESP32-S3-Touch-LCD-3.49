#!/usr/bin/env python3
"""
make_sounds.py — звуки HUD в C-массивы (PCM 16 бит, моно, 22050 Гц)
------------------------------------------------------------------------------
Источник: tools/sounds/<имя>.mp3|.wav|.ogg  ->  hud_sounds.c / hud_sounds.h
Выход: waveshare_hud_mockup/hud_sounds.c / hud_sounds.h
Нужен ffmpeg в PATH. Запуск из корня репозитория:  python tools/make_sounds.py

Обработка: моно, 22050 Гц, обрезка тишины в конце, нормализация пика до -1 дБ,
плавное затухание последних 30 мс (без щелчка в конце).
"""
import os, subprocess, struct, glob

HERE = os.path.dirname(os.path.abspath(__file__))
SKETCH = os.path.abspath(os.path.join(HERE, "..", "waveshare_hud_mockup"))
SR = 22050

def convert(path):
    cmd = ["ffmpeg", "-v", "error", "-i", path, "-ac", "1", "-ar", str(SR),
           "-af", "areverse,silenceremove=start_periods=1:start_threshold=-50dB,areverse",
           "-f", "s16le", "-"]
    raw = subprocess.run(cmd, check=True, capture_output=True).stdout
    s = list(struct.unpack("<%dh" % (len(raw) // 2), raw))
    peak = max(1, max(abs(v) for v in s))
    g = (32767 * 0.89) / peak                       # -1 дБ
    s = [int(max(-32768, min(32767, v * g))) for v in s]
    fade = int(SR * 0.03)
    for i in range(min(fade, len(s))):
        s[-1 - i] = int(s[-1 - i] * i / fade)
    return s

def main():
    srcs = sorted(glob.glob(os.path.join(HERE, "sounds", "*.*")))
    out_c = ["/* Сгенерировано tools/make_sounds.py — не править вручную */",
             '#include "hud_sounds.h"', ""]
    out_h = ["/* Сгенерировано tools/make_sounds.py — не править вручную */",
             "#ifndef HUD_SOUNDS_H", "#define HUD_SOUNDS_H", "#include <stdint.h>", "",
             f"#define HUD_SOUND_RATE {SR}", ""]
    for p in srcs:
        name = os.path.splitext(os.path.basename(p))[0]
        s = convert(p)
        rows = [", ".join(str(v) for v in s[i:i + 16]) for i in range(0, len(s), 16)]
        out_c.append(f"const int16_t hud_snd_{name}[{len(s)}] = {{\n    " + ",\n    ".join(rows) + "\n};")
        out_c.append(f"const uint32_t hud_snd_{name}_len = {len(s)};\n")
        out_h.append(f"extern const int16_t  hud_snd_{name}[];      /* {len(s) / SR:.2f} с */")
        out_h.append(f"extern const uint32_t hud_snd_{name}_len;")
        print(f"{name}: {len(s) / SR:.2f} с, {len(s) * 2 // 1024} КБ")
    out_h += ["", "#endif", ""]
    open(os.path.join(SKETCH, "hud_sounds.c"), "w").write("\n".join(out_c))
    open(os.path.join(SKETCH, "hud_sounds.h"), "w").write("\n".join(out_h))

if __name__ == "__main__":
    main()
