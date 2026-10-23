#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_cams.py — конвертер базы камер в компактный cams.bin для HUD.

Основной источник — speedcamonline.ru, формат iGo extended, без мобильных:
    https://speedcamonline.ru/igoext/Rus/nomobile/
CSV с заголовком `IDX,X,Y,TYPE,SPEED,DIRTYPE,DIRECTION` (X — долгота, Y — широта).
TYPE: 192/194 — стационарная камера, 13 — камера скорости (в т.ч. по разметке), 206 — пост ДПС, 193/199 — мобильная/тренога
(в nomobile нет), 68 — не расшифрован. SPEED — лимит км/ч (0 — неизвестен).
DIRECTION в iGo повёрнут на 180° относительно «направления движения», поэтому конвертер прибавляет 180°.
Битые/оборванные строки (файл с сайта иногда обрывается) пропускаются.

Дополнительно можно подмешать базу Datakam (speedcam_*.bin): её записи кодов 510 и 760 добавляются,
только если рядом (≤ 30 м) в основной базе нет записи того же типа. Формат Datakam: docs/cams_format.md.

Также читает 7Ways+ extended+ (openSpeedcam.net, 9 колонок `IDX,X,Y,TYPE,SPEED,DirType,Direction,Distance,Angle`),
формат определяется автоматически; берутся только камеры (W7_TYPES), Direction 0/360 = без направления.

Формат выхода cams.bin — docs/cams_format.md.

Запуск:
  python3 gen_cams.py igo.csv                        # -> cams.bin
  python3 gen_cams.py igo.csv speedcam_D6.bin -o cams.bin --bbox 55.0 36.5 56.3 38.5
Формат входа определяется по содержимому. iGo / 7Ways+ файлы (например, RU + BY) объединяются целиком, дубли ближе 10 м
того же типа отбрасываются; Datakam (.bin) — только дополнение (коды 510 и 760, см. выше).
"""
import struct, sys, re, os, math, argparse, time

CELL = 20                                   # ячеек на градус (0.05°)
DK_CODES = [510, 530, 700, 710, 720, 730, 740, 750, 760]   # индексы 0..8 (как у Datakam)
IGO_TYPES = {192: 0, 194: 0, 206: 8, 68: 1, 193: 9, 199: 10}  # iGo TYPE -> индекс типа в cams.bin
# 7Ways+ (openSpeedcam.net) -> индекс типа. Только камеры; знаки, переходы, лежачие и пр. не берём.
# 192/194 стационарная/красный свет, 68 камера на светофоре, 215 пост ДПС, 193/209 мобильная,
# 227 начало средней скорости, 199 камера полосы (автобусной).
W7_TYPES = {192: 0, 194: 0, 13: 0, 68: 1, 215: 8, 193: 9, 209: 9, 227: 12, 199: 11, 12: 0, 203: 0}
W7_FLAG = {12: 1, 203: 2}      # признак в cams.bin (биты 4..5): 1 — камера скорости «сзади» (12), 2 — светофор + скорость (203)
W7_DIR_OFFSET = 180     # поворот Direction (град.): в 7Ways+ Direction — направление «на камеру», по логам поездок нужен разворот на 180°
MERGE_M = 30.0


def read_igo(path):
    """-> (список (lat, lon, dir|None, lim, type_idx, flag), дата ддммгг, число строк)"""
    out, n = [], 0
    with open(path, encoding='utf-8', errors='ignore') as f:
        for line in f:
            p = line.strip().split(',')
            if not p or p[0] == 'IDX' or len(p) != 7: continue
            n += 1
            try:
                lon, lat = float(p[1]), float(p[2])
                t, lim, dtype, d = int(p[3]), int(p[4]), int(p[5]), int(p[6])
            except ValueError:
                continue
            if t not in IGO_TYPES: continue
            hd = None if (dtype == 0 or d >= 360) else (d + 180) % 360
            out.append((lat, lon, hd, min(max(lim, 0), 255), IGO_TYPES[t], 0))
    d = time.localtime(os.path.getmtime(path))
    return out, int('%02d%02d%02d' % (d.tm_mday, d.tm_mon, d.tm_year % 100)), n


def read_7ways(path):
    """7Ways+ extended+: IDX,X,Y,TYPE,SPEED,DirType,Direction,Distance,Angle (9 колонок)."""
    out, n = [], 0
    date = 0
    with open(path, encoding='utf-8', errors='ignore') as f:
        for line in f:
            if line.startswith('IDX'):
                m = re.search(r'(\d\d)\.(\d\d)\.(\d{4})', line)
                if m: date = int(m.group(1) + m.group(2) + m.group(3)[2:])
                continue
            p = line.strip().split(',')
            if len(p) != 9: continue
            n += 1
            try:
                lon, lat = float(p[1]), float(p[2])
                t, lim, dtype, d = int(p[3]), int(p[4]), int(p[5]), int(p[6])
            except ValueError:
                continue
            if t not in W7_TYPES: continue
            lim = min(max(lim, 0), 255); fl = W7_FLAG.get(t, 0)
            if dtype == 0 or d <= 0 or d >= 360:
                out.append((lat, lon, None, lim, W7_TYPES[t], fl))
            else:
                hd = (d + W7_DIR_OFFSET) % 360
                out.append((lat, lon, hd, lim, W7_TYPES[t], fl))
                if dtype == 2:                                  # DirType 2 — в обе стороны: вторая запись на обратный курс
                    out.append((lat, lon, (hd + 180) % 360, lim, W7_TYPES[t], fl))
    if not date:
        d = time.localtime(os.path.getmtime(path))
        date = int('%02d%02d%02d' % (d.tm_mday, d.tm_mon, d.tm_year % 100))
    return out, date, n


def read_datakam(path):
    b = open(path, 'rb').read()
    m = re.search(r'(\d\d)\.(\d\d)\.(\d\d)', b[:64].decode('cp1251', 'ignore'))
    date = int(m.group(1) + m.group(2) + m.group(3)) if m else 0
    n = struct.unpack_from('<I', b, 64)[0]
    if 68 + n * 23 != len(b): sys.exit(path + ': размер не сходится с числом записей — формат другой?')
    out = []
    for i in range(n):
        r = b[68 + i * 23: 68 + (i + 1) * 23]
        lon, lat = struct.unpack_from('<ff', r, 0)
        code, lim, hd = struct.unpack_from('<HHH', r, 8)
        t = DK_CODES.index(code) if code in DK_CODES else 15
        out.append((lat, lon, None if hd >= 360 else hd, min(lim, 255), t, r[18] & 3))
    return out, date, n


def is_igo(path):
    with open(path, 'rb') as f: return f.read(4) in (b'IDX,', b'\xef\xbb\xbf')


def is_7ways(path):
    with open(path, encoding='utf-8-sig', errors='ignore') as f:
        return f.readline().startswith('IDX') and f.readline().count(',') == 8


def read_any(path):
    if is_7ways(path): return read_7ways(path)
    return read_igo(path) if is_igo(path) else read_datakam(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('inputs', nargs='+', help='iGo-ext / 7Ways+ файлы (можно несколько стран), затем, при желании, speedcam_*.bin Datakam')
    ap.add_argument('-o', '--out', default='cams.bin')
    ap.add_argument('--bbox', nargs=4, type=float, metavar=('LAT0', 'LON0', 'LAT1', 'LON1'))
    ap.add_argument('--w7-dir-offset', type=int, default=None, metavar='DEG', help='поворот Direction 7Ways+ в градусах (по умолчанию 180; 0 — без поворота)')
    ap.add_argument('--exclude', metavar='FILE', help='файл со списком ложных камер: «lat, lon [радиус_м]» в строке (по умолчанию 15 м), # — комментарий')
    a = ap.parse_args()
    if a.w7_dir_offset is not None:
        global W7_DIR_OFFSET
        W7_DIR_OFFSET = a.w7_dir_offset

    recs, date, total = [], 0, 0
    grid = {}                                   # для проверки «рядом уже есть» при подмешивании
    for k, path in enumerate(a.inputs):
        dk = not is_7ways(path) and not is_igo(path)            # Datakam — только дополнение; iGo / 7Ways+ — полноценные источники
        rows, d, n = read_any(path)
        total += n; date = date or d
        added = 0
        for r in rows:
            lat, lon = r[0], r[1]
            if not (19 < lon < 180 and 40 < lat < 82): continue
            if a.bbox and not (a.bbox[0] <= lat <= a.bbox[2] and a.bbox[1] <= lon <= a.bbox[3]): continue
            if k > 0:
                if dk and r[4] not in (0, 8): continue          # из Datakam берём только 510 и 760
                merge_m = MERGE_M if dk else 10.0               # iGo / 7Ways+ (например, RU + BY): отбрасываем лишь дубли в 10 м
                cx, cy = int(lon * 100), int(lat * 100)         # ячейка ~1 км
                dup = False
                for dx in (-1, 0, 1):
                    for dy in (-1, 0, 1):
                        for q in grid.get((cx + dx, cy + dy), ()):
                            if q[4] == r[4]:
                                dy_m = (q[0] - lat) * 111200
                                dx_m = (q[1] - lon) * 111200 * math.cos(math.radians(lat))
                                if dx_m * dx_m + dy_m * dy_m <= merge_m ** 2: dup = True
                if dup: continue
            recs.append(r); added += 1
            grid.setdefault((int(lon * 100), int(lat * 100)), []).append(r)
        print(f'{path}: {added} записей добавлено из {n}')
    if a.exclude:
        ex = []
        for ln in open(a.exclude, encoding='utf-8'):
            ln = ln.split('#')[0].replace(',', ' ').split()
            if len(ln) >= 2: ex.append((float(ln[0]), float(ln[1]), float(ln[2]) if len(ln) > 2 else 15.0))
        def bad(r):
            return any(math.hypot((r[0] - la) * 111200, (r[1] - lo) * 111200 * math.cos(math.radians(la))) <= rad for la, lo, rad in ex)
        n0 = len(recs); recs = [r for r in recs if not bad(r)]
        print(f'исключено по {a.exclude}: {n0 - len(recs)}')
    f32 = lambda v: struct.unpack('<f', struct.pack('<f', v))[0]     # ключ считается по float32, как в файле
    recs.sort(key=lambda r: (int(f32(r[0]) * CELL), int(f32(r[1]) * CELL)))
    with open(a.out, 'wb') as f:
        f.write(b'CAM1' + struct.pack('<III', len(recs), date, 0))
        for r in recs:
            f.write(struct.pack('<ffHBB', r[0], r[1], 0xFFFF if r[2] is None else r[2], r[3], r[4] | r[5] << 4))
    print(f'{a.out}: {len(recs)} камер, {16 + 12 * len(recs)} байт, база от {date:06d}')


if __name__ == '__main__': main()
