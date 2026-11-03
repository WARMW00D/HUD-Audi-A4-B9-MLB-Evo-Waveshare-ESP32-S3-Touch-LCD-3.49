#!/usr/bin/env python3
"""
gen_arrow_anim.py — пакует анимированные стрелки навигации (кадры PNG) в arrows.bin для FATFS.

    pip install pillow numpy lz4
    python tools/gen_arrow_anim.py <папка_с_кадрами> [arrows.bin]

Папка с кадрами: <папка>/large_sport/<имя>.p<N>.png (+ blank.png — пустой фон и frames.idx со строками
«имя<TAB>число_кадров»). Кадр 328x180 (вид с перспективой и сеткой пола).
Кадр 328x180: содержимое (дорога, стрелка, кольцо) занимает только прямоугольник CROP (192x141, общий для всех кадров),
он вырезается и по высоте сжимается до H (192x130): сверху остаётся место под бар прогресса, снизу — под строку
дистанции. Кадров "approach" (движение прямо) всего 13 — недостающие досчитываются смешиванием соседних (до 36).

Пол с сеткой одинаков во всех кадрах (blank.png) и в файл НЕ кладётся: в кадрах остаётся только то, что от него
отличается (дорога, стрелка, кольцо...), пиксели, совпадающие с полом, прозрачны — анимация рисуется на чёрном.
Кадры — общая палитра 16 цветов (индекс 0 = прозрачный), 4 бита на пиксель, каждый кадр сжат отдельно блоком
LZ4: любой кадр распаковывается независимо от остальных (произвольный доступ, прыжки по манёвру).

Формат файла (little-endian):
  0   "ANM1"
  4   u16 W, u16 H
  8   u16 число анимаций, u16 0
  12  u32 размер файла
  16  u32 0 (зарезервировано; раньше — смещение фона)
  20  палитра кадров 16 x 4 байта (B, G, R, A) — формат lv_color32_t для LV_IMG_CF_INDEXED_4BIT
  84  таблица анимаций: N x 32 байта: имя[24] (0-дополнено), u16 кадров, u16 0, u32 номер первого кадра
  ... таблица смещений кадров: M x u32 (от начала файла)
  ... кадры: u16 длина сжатого блока + блок LZ4 (распакованный размер = ((W+1)//2)*H)
"""
import os, struct, sys
import numpy as np
import lz4.block
from PIL import Image

W, H = 192, 130
CROP = (68, 39, 260, 180)             # общая рамка содержимого во всех кадрах 328x180 (по всем кадрам)
SMOOTH = {"approach": 36}             # анимации с малым числом кадров: досчитать до N смешиванием соседних (по кругу)
SKIP = {"merge_l", "merge_r", "turn_r000"}   # в прошивке не используются; «прямо» рисует approach (без скруглённого тупика)
RAW = ((W + 1) // 2) * H
DIFF = 2                               # пиксель отличается от фона больше, чем на это — рисуется

def fit(im):
    """328x180 -> вырезка CROP и масштаб до W x H (по высоте сжатие)."""
    return np.array(im.crop(CROP).resize((W, H), Image.LANCZOS)).astype(np.int32)


def main():
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(src, "arrows.bin")
    d = os.path.join(src, "large_sport")
    blank_im = Image.open(os.path.join(d, "blank.png")).convert("RGB")
    bg = fit(blank_im)
    names = []
    for line in open(os.path.join(d, "frames.idx"), encoding="utf-8"):
        p = line.split()
        if len(p) == 2 and p[0] not in SKIP:
            names.append((p[0], int(p[1])))

    cache = {}

    def raw(n, i):
        if (n, i) in cache:
            return cache[(n, i)]
        c = fit(Image.open(f"{d}/{n}.p{i}.png").convert("RGB"))
        if n in SMOOTH:
            cache[(n, i)] = c
        return c

    def load(n, i):
        k0 = dict(names)[n]
        k = SMOOTH.get(n, k0)
        if k == k0:
            return raw(n, i)
        t = i * k0 / k                       # позиция в исходных кадрах, цикл
        a = int(t) % k0
        f = t - int(t)
        return np.rint(raw(n, a) * (1 - f) + raw(n, (a + 1) % k0) * f).astype(np.int32)

    def mask_of(c):
        return (np.abs(c - bg).max(2) > DIFF)

    # палитра кадров по выборке
    pix = []
    for n, k in names:
        for i in {0, k // 2, k - 1}:
            c = raw(n, i)
            pix.append(c[mask_of(c)][::2])
    pix = np.concatenate(pix).astype(np.uint8)
    side = int(len(pix) ** 0.5)
    pim = Image.fromarray(pix[: side * side].reshape(side, side, 3))
    pal15 = np.array(pim.quantize(15, method=Image.Quantize.MEDIANCUT).getpalette()[:45]).reshape(15, 3)
    pal = np.vstack([[0, 0, 0], pal15]).astype(np.int32)

    def pack(idx):
        f = idx.reshape(-1)
        return ((f[0::2] << 4) | f[1::2]).astype(np.uint8).tobytes()           # старший ниббл — левый пиксель

    def lz(b):
        blk = lz4.block.compress(b, mode="high_compression", compression=12, store_size=False)
        assert len(blk) < 65536
        return struct.pack("<H", len(blk)) + blk

    frames, anim = [], []
    for n, k in names:
        k = SMOOTH.get(n, k)
        anim.append((n, k, len(frames)))
        for i in range(k):
            c = load(n, i)
            dist = ((c[:, :, None, :] - pal[None, None, :, :]) ** 2).sum(3)
            idx = dist.argmin(2).astype(np.uint8)
            idx[~mask_of(c)] = 0
            frames.append(lz(pack(idx)))

    head = 20 + 64
    offs_at = head + len(anim) * 32
    o = offs_at + len(frames) * 4
    offs = []
    for f in frames:
        offs.append(o)
        o += len(f)
    size = o
    b = bytearray()
    b += b"ANM1" + struct.pack("<HHHHII", W, H, len(anim), 0, size, 0)
    for r, g, bl in pal:
        b += bytes((int(bl), int(g), int(r), 0 if i0(r, g, bl) else 255))
    for n, k, first in anim:
        b += n.encode().ljust(24, b"\0") + struct.pack("<HHI", k, 0, first)
    for x in offs:
        b += struct.pack("<I", x)
    for f in frames:
        b += f
    assert len(b) == size
    open(out, "wb").write(b)
    print(f"{out}: {len(anim)} анимаций, {len(frames)} кадров, {size / 1024:.0f} КБ")


def i0(r, g, b):
    return r == 0 and g == 0 and b == 0


if __name__ == "__main__":
    main()
