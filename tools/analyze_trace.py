"""Analyzes a WPR recording (FileIO + DiskIO) that was converted to CSV with tracerpt.

Record (admin PowerShell):  wpr -start FileIO -start DiskIO -filemode
                            ... play for 10-15 minutes ...
                            wpr -stop game.etl
Convert:                    tracerpt game.etl -o game.csv -of CSV
Analyze:                    python -I analyze_trace.py game.csv .pak [block-kb]

The second argument is a substring of the game's data file paths (e.g. the extension).
Writes a report to stdout and hot_ranges.json (ranges read per file) next to the CSV.

gamecache helps if most reads bypass the Windows cache (IRP_NOCACHE) and the same data
is read several times.
"""
import csv
import json
import os
import sys
from collections import Counter, defaultdict

CSV_PATH = sys.argv[1]
PATH_FILTER = (sys.argv[2] if len(sys.argv) > 2 else ".rmdblob").lower()
BLOCK = (int(sys.argv[3]) if len(sys.argv) > 3 else 64) * 1024

FILE_NO_INTERMEDIATE_BUFFERING = 0x8
IRP_NOCACHE = 0x1
IRP_PAGING_IO = 0x2


def i(v):
    v = v.strip()
    return int(v, 16) if v.lower().startswith("0x") else int(v)


fo_path = {}          # FileObject -> path (from Create)
fk_path = {}          # FileKey -> path (from FileRundown)
open_opts = Counter() # (pid, unbuffered) -> opens of matching files
reads_by_pid = Counter()
read_count = read_bytes = read_nocache = read_paging = 0
read_sizes = Counter()
block_hits = defaultdict(Counter)
per_file_bytes = Counter()
first_ts = last_ts = None
minute_bytes = Counter()
minute_new_blocks = Counter()
disk_reads = disk_bytes = 0

with open(CSV_PATH, newline="", encoding="utf-8", errors="replace") as fh:
    for line in fh:
        head = line[:20].lstrip()
        if not (head.startswith("FileIo") or head.startswith("DiskIo")):
            continue
        row = next(csv.reader([line], skipinitialspace=True))
        ev, typ = row[0].strip(), row[1].strip()
        try:
            ts = int(row[16])
        except (ValueError, IndexError):
            continue
        if first_ts is None:
            first_ts = ts
        last_ts = ts

        if ev == "FileIo" and typ == "Create":
            path = row[25].strip().strip('"')
            fo_path[row[20].strip()] = path
            if PATH_FILTER in path.lower():
                opts = i(row[22]) & 0xFFFFFF
                open_opts[(i(row[9]), bool(opts & FILE_NO_INTERMEDIATE_BUFFERING))] += 1
        elif ev == "FileIo" and typ == "FileRundown":
            fk_path[row[19].strip()] = row[20].strip().strip('"')
        elif ev == "FileIo" and typ == "Read":
            path = fo_path.get(row[21].strip()) or fk_path.get(row[22].strip())
            if not path or PATH_FILTER not in path.lower():
                continue
            off, size, flags = i(row[19]), i(row[24]), i(row[25])
            read_count += 1
            read_bytes += size
            reads_by_pid[i(row[9])] += 1
            read_nocache += bool(flags & IRP_NOCACHE)
            read_paging += bool(flags & IRP_PAGING_IO)
            read_sizes[size] += 1
            key = path.lower()
            per_file_bytes[key] += size
            minute = (ts - first_ts) // 600_000_000
            minute_bytes[minute] += size
            hits = block_hits[key]
            for b in range(off // BLOCK, (off + max(size, 1) - 1) // BLOCK + 1):
                if b not in hits:
                    minute_new_blocks[minute] += 1
                hits[b] += 1
        elif ev == "DiskIo" and typ == "Read":
            disk_reads += 1
            disk_bytes += i(row[21])

GB = 1024 ** 3
dur_min = (last_ts - first_ts) / 600_000_000 if first_ts else 0
unique_blocks = sum(len(h) for h in block_hits.values())
reread_blocks = sum(1 for h in block_hits.values() for c in h.values() if c > 1)

print(f"Recording: {dur_min:.1f} min, filter: '{PATH_FILTER}', block size {BLOCK // 1024} KB")
print("\n== Opens of matching files (PID, unbuffered?) ==")
for (pid, nobuf), n in open_opts.most_common():
    print(f"  PID {pid:>6}  unbuffered={nobuf!s:<5}  {n}x")
print("\n== Reads (file level) ==")
print(f"  count: {read_count:,}  volume: {read_bytes / GB:.2f} GB")
if read_count:
    print(f"  bypassing the Windows cache (IRP_NOCACHE): {read_nocache / read_count:.1%}")
    print(f"  paging I/O (cache / memory mapping): {read_paging / read_count:.1%}")
print("  reading processes:", ", ".join(f"PID {p}: {n:,}" for p, n in reads_by_pid.most_common(5)))
print("  most common sizes:", ", ".join(f"{s // 1024} KB: {n:,}" for s, n in read_sizes.most_common(8)))
print(f"\n  distinct data touched: {unique_blocks * BLOCK / GB:.2f} GB")
print(f"  of which read more than once: {reread_blocks * BLOCK / GB:.2f} GB")
if unique_blocks:
    print(f"  read volume / distinct data: {read_bytes / (unique_blocks * BLOCK):.2f}x")
print("\n== Per minute: read / newly touched (GB) ==")
for m in sorted(minute_bytes):
    print(f"  min {m:>3}: {minute_bytes[m] / GB:6.2f} / {minute_new_blocks[m] * BLOCK / GB:6.2f}")
print("\n== Per file: read / touched ==")
for key, b in per_file_bytes.most_common(25):
    print(f"  {key.rsplit(chr(92), 1)[-1]:<40} {b / GB:6.2f} GB  {len(block_hits[key]) * BLOCK / GB:6.2f} GB")
print("\n== Disk level (all processes) ==")
print(f"  disk reads: {disk_reads:,}, {disk_bytes / GB:.2f} GB")

out = {}
for key, hits in block_hits.items():
    spans, start, prev = [], None, None
    for b in sorted(hits):
        if start is None:
            start = prev = b
        elif b == prev + 1:
            prev = b
        else:
            spans.append([start * BLOCK, (prev + 1) * BLOCK])
            start = prev = b
    if start is not None:
        spans.append([start * BLOCK, (prev + 1) * BLOCK])
    out[key] = spans
with open(os.path.join(os.path.dirname(os.path.abspath(CSV_PATH)), "hot_ranges.json"), "w") as fh:
    json.dump({"block": BLOCK, "files": out}, fh)
