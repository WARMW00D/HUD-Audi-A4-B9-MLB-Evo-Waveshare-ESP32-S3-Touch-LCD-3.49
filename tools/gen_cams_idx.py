#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_cams_idx.py — cams.bin + индексный файл cams.idx (для чтения базы с SD-карты без загрузки в RAM).

Формат индекса — docs/cams_index.md. Формат cams.bin и источники — как у gen_cams.py (он лежит рядом и вызывается).

Запуск:
  python3 gen_cams_idx.py igo.csv                         # -> cams.bin + cams.idx
  python3 gen_cams_idx.py igo.csv speedcam_D6.bin -o cams.bin --bbox 55.0 36.5 56.3 38.5
  python3 gen_cams_idx.py --from-bin cams.bin             # индекс для уже готового cams.bin
Индекс всегда строится по итоговому cams.bin, поэтому всегда с ним согласован.
"""
import struct, sys, os, subprocess, zlib, argparse

CELL = 20


def build_index(bin_path, idx_path):
    b = open(bin_path, 'rb').read()
    if b[:4] != b'CAM1': sys.exit(bin_path + ': не cams.bin (нет сигнатуры CAM1)')
    n, date = struct.unpack_from('<II', b, 4)
    if 16 + 12 * n != len(b): sys.exit(bin_path + ': размер не сходится с числом записей')
    cells, prev = [], None                       # [(key, номер первой записи)]
    for i in range(n):
        lat, lon = struct.unpack_from('<ff', b, 16 + 12 * i)
        key = int(lat * CELL) * 4096 + int(lon * CELL)
        if prev is not None and key < prev: sys.exit(f'{bin_path}: запись {i} нарушает сортировку')
        if key != prev: cells.append((key, i)); prev = key
    cells.append((0xFFFFFFFF, n))                # завершающая запись: конец последней ячейки
    with open(idx_path, 'wb') as f:
        f.write(b'CIX1' + struct.pack('<III', len(cells) - 1, n, zlib.crc32(b) & 0xFFFFFFFF))
        for k, first in cells: f.write(struct.pack('<II', k, first))
    size = 16 + 8 * len(cells)
    print(f'{idx_path}: {len(cells) - 1} непустых ячеек, {size} байт '
          f'(cams.bin: {n} записей, {len(b)} байт, база от {date:06d})')
    return b, cells


def self_check(b, cells):
    """каждая запись лежит в своей ячейке, число записей в ячейках сходится с N"""
    total = 0
    for (k, first), (_, nxt) in zip(cells, cells[1:]):
        for i in range(first, nxt):
            lat, lon = struct.unpack_from('<ff', b, 16 + 12 * i)
            assert int(lat * CELL) * 4096 + int(lon * CELL) == k, f'запись {i} не в своей ячейке'
        total += nxt - first
    assert total == (len(b) - 16) // 12, 'сумма по ячейкам != N'
    print('проверка индекса: ок')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('inputs', nargs='*', help='iGo-ext CSV, затем, при желании, speedcam_*.bin Datakam')
    ap.add_argument('-o', '--out', default='cams.bin')
    ap.add_argument('--idx', help='имя индекса (по умолчанию рядом с cams.bin, расширение .idx)')
    ap.add_argument('--bbox', nargs=4, type=float, metavar=('LAT0', 'LON0', 'LAT1', 'LON1'))
    ap.add_argument('--from-bin', metavar='CAMS_BIN', help='только индекс для готового cams.bin')
    a = ap.parse_args()

    if a.from_bin: bin_path = a.from_bin
    else:
        if not a.inputs: ap.error('нужен входной файл или --from-bin')
        cmd = [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gen_cams.py'),
               *a.inputs, '-o', a.out]
        if a.bbox: cmd += ['--bbox', *map(str, a.bbox)]
        subprocess.run(cmd, check=True)
        bin_path = a.out
    b, cells = build_index(bin_path, a.idx or os.path.splitext(bin_path)[0] + '.idx')
    self_check(b, cells)


if __name__ == '__main__': main()
