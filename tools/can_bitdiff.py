#!/usr/bin/env python3
"""
can_bitdiff.py — какие биты кадра меняются во времени (по логу сниффера)
------------------------------------------------------------------------------
Формат лога: <millis> <S|X> <ID hex> [R]<DLC> <байты...>   (.txt или .txt.lzma)

  python3 can_bitdiff.py can_log_0012.txt.lzma 0x397
  python3 can_bitdiff.py can_log_0012.txt.lzma 0x31E --ignore 0-11
  python3 can_bitdiff.py log.txt 0x397 0x31E --from 30 --to 60

Печатает только кадры, где данные изменились (кроме битов из --ignore:
CRC/счётчик), с номерами изменившихся битов (Intel: бит = байт*8 + бит_в_байте)
и значением этих бит. В конце — сводка: какие биты вообще менялись.
"""
import argparse, lzma, sys

def open_log(path):
    return lzma.open(path, "rt", errors="replace") if path.endswith(".lzma") else open(path, errors="replace")

def parse_ignore(spec):
    out = set()
    for part in (spec or "").split(","):
        if not part: continue
        a, _, b = part.partition("-")
        out.update(range(int(a), int(b or a) + 1))
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("ids", nargs="+")
    ap.add_argument("--ignore", default="", help="биты, которые не учитывать, напр. 0-11")
    ap.add_argument("--from", dest="t0", type=float, default=None, help="с какой секунды")
    ap.add_argument("--to", dest="t1", type=float, default=None, help="до какой секунды")
    a = ap.parse_args()
    ids = {int(x, 16) for x in a.ids}
    ign = parse_ignore(a.ignore)
    last, t_first, changed_any, count = {}, None, {i: set() for i in ids}, {i: 0 for i in ids}

    for line in open_log(a.log):
        p = line.split()
        if len(p) < 4 or p[1] not in ("S", "X"): continue
        try:
            ms = int(p[0]); cid = int(p[2], 16)
        except ValueError:
            continue
        if cid not in ids: continue
        dlc_s = p[3]
        if dlc_s.startswith("R"): continue
        data = bytes(int(x, 16) for x in p[4:4 + int(dlc_s)])
        if t_first is None: t_first = ms
        t = (ms - t_first) / 1000
        if a.t0 is not None and t < a.t0: continue
        if a.t1 is not None and t > a.t1: break
        count[cid] += 1
        raw = int.from_bytes(data.ljust(8, b"\0"), "little")
        prev = last.get(cid)
        last[cid] = raw
        if prev is None:
            print(f"{t:9.2f}  0x{cid:03X}  {data.hex(' ')}  (первый кадр)")
            continue
        diff = (raw ^ prev)
        bits = [b for b in range(64) if diff >> b & 1 and b not in ign]
        if not bits: continue
        changed_any[cid].update(bits)
        desc = " ".join(f"{b}:{raw >> b & 1}" for b in bits)
        print(f"{t:9.2f}  0x{cid:03X}  {data.hex(' ')}  изм. биты {desc}")

    print("\nСводка:")
    for cid in sorted(ids):
        print(f"  0x{cid:03X}: кадров {count[cid]}, менялись биты {sorted(changed_any[cid]) or '—'}")

if __name__ == "__main__":
    main()
