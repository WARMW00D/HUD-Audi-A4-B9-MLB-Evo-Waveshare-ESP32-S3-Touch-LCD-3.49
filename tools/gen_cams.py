#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_cams.py — конвертер базы камер Datakam (speedcam_*.bin) в компактный cams.bin для HUD.

Вход (формат Datakam, найден разбором файла):
  0..63   текстовый заголовок "DATAKAM. MOSCOW. SPEEDCAM BASE.\r\n СОЗДАН ОТ: дд.мм.гг\r\n\r\n"
  64..67  u32 число записей N
  68..    N записей по 23 байта, little-endian:
      0  f32 долгота          4  f32 широта
      8  u16 код типа (510,530,700..760). Сверка с размеченным списком (cams.txt, ~1270 точек,
         совпадение до 30 м): 510 = стационарная камера (СТЦ; муляжи МЛЖ, ММ, ДПСкам идут тем же кодом),
         760 = пост ДПС / ДПС мобильный; 700, 710, 720 — по cams.bkm объекты с дальностью 50 м (не камеры скорости,
         вероятно неровности / переезды); 530 — всего 4 точки типа 18952, не расшифрован; 730, 740, 750 — не расшифрованы.
         Мобильные (тренога, ДПСмоб, МЗ) в Datakam разбросаны по 510/760/530/700/750 — отделить их нельзя
     10  u16 ограничение, км/ч
     12  u16 направление, ° (360 = без направления)
     14  u16 радиус оповещения, м (всегда 500)
     16  u16 константа 10
     18  u8  признак 0/1/2
     19  u32 ключ (по нему база отсортирована у Datakam), HUD не использует
  Проверено по логу: курс машины при проезде записи совпадает с её направлением.

Выход cams.bin (little-endian), записи отсортированы по ячейке сетки 0.05°:
  заголовок 16 байт: "CAM1", u32 N, u32 дата (ддммгг числом), u32 резерв
  запись 12 байт:   f32 lat, f32 lon, u16 направление (0xFFFF — нет), u8 лимит км/ч,
                    u8 (индекс типа 0..8 | признак<<4)
Запуск:  python3 gen_cams.py speedcam_D6.bin [cams.bin] [--bbox lat0 lon0 lat1 lon1]
"""
import struct, sys, re, math

CODES = [510, 530, 700, 710, 720, 730, 740, 750, 760]
CELL = 20      # ячеек на градус (0.05°)

def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    if not a: sys.exit(__doc__)
    src = a[0]; dst = a[1] if len(a) > 1 and not re.match(r'^-?\d', a[1]) else 'cams.bin'
    bbox = None
    if '--bbox' in sys.argv:
        k = sys.argv.index('--bbox'); bbox = [float(x) for x in sys.argv[k+1:k+5]]
        if dst == sys.argv[k+1]: dst = 'cams.bin'
    b = open(src, 'rb').read()
    head = b[:64].decode('cp1251', 'ignore')
    m = re.search(r'(\d\d)\.(\d\d)\.(\d\d)', head)
    date = int(m.group(1) + m.group(2) + m.group(3)) if m else 0
    n = struct.unpack_from('<I', b, 64)[0]
    if 68 + n * 23 != len(b): sys.exit('размер файла не сходится с числом записей — формат другой?')
    out = []
    for i in range(n):
        r = b[68 + i * 23: 68 + (i + 1) * 23]
        lon, lat = struct.unpack_from('<ff', r, 0)
        code, lim, hd = struct.unpack_from('<HHH', r, 8)
        fl = r[18]
        if not (19 < lon < 180 and 40 < lat < 82): continue
        if bbox and not (bbox[0] <= lat <= bbox[2] and bbox[1] <= lon <= bbox[3]): continue
        t = CODES.index(code) if code in CODES else 15
        out.append((int(lat * CELL), int(lon * CELL), lat, lon, 0xFFFF if hd >= 360 else hd, min(lim, 255), t | (fl & 3) << 4))
    out.sort(key=lambda x: (x[0], x[1]))
    with open(dst, 'wb') as f:
        f.write(b'CAM1' + struct.pack('<III', len(out), date, 0))
        for o in out: f.write(struct.pack('<ffHBB', o[2], o[3], o[4], o[5], o[6]))
    print(f'{dst}: {len(out)} камер из {n}, {16 + 12 * len(out)} байт, база от {date:06d}')

if __name__ == '__main__': main()
