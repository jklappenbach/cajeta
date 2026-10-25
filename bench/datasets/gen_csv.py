#!/usr/bin/env python3
"""Generate the codec/csv-parse and codec/csv-write input (bench/codec/csv/Csv.md).

    gen_csv.py <dir> [full|smoke]

Writes <dir>/codec-csv-<scale>.csv and <dir>/codec-csv-<scale>.meta. The file is
deterministic (xorshift64, fixed seed), so every language reads the same bytes;
the SHA-256 below is checked after writing, the way datasets/fetch.sh checks a
download, and an existing file whose digest matches is reused.

Shape: header `id,name,qty,price,note`, then rows of
  id     int64, 1..N
  name   String; one row in four is "Last, First" (quoted: embedded comma)
  qty    int32, 0..9999
  price  float64 with exactly two decimals whose last digit is nonzero, so the
         shortest round-trip form every standard formatter prints is the
         same text that was read
  note   String; one row in eight carries embedded quotes and a comma
         ("said ""hi"", then left"), one in eight a bare comma, the rest plain
Fields are quoted only when RFC 4180 requires it (a comma, quote or newline),
no field is empty or starts with a space, and every record ends in "\\n", so a
minimal-quoting writer reproduces the input byte for byte (csv-write's check).

full is the page's 100 MB (>= 100,000,000 bytes); smoke is 4 MB, used when
CAJETA_BENCH_SMOKE=1 and noted on every smoke row.

The .meta file is one line, "rows qty_sum bytes": the expected row count, the
sum of the qty column (the typed-field checksum) and the file size.
"""
import hashlib
import os
import sys

TARGET = {"full": 100_000_000, "smoke": 4_000_000}
# Recorded from the first generation (2026-10-25); a change to this generator
# must change these, which is the point.
SHA256 = {
    "full": "f6a50cdea5dfe101a17183c61ba37217966c365d7c704bf216ee1419dc3a26f1",
    "smoke": "ec53babe55df8bf6b1faaf0301fb3038ef956814d98b29678e04746a50c2885c",
}

FIRST = ["James", "Mary", "Robert", "Patricia", "John", "Jennifer", "Michael",
         "Linda", "David", "Elizabeth", "William", "Barbara", "Richard", "Susan",
         "Joseph", "Jessica", "Thomas", "Sarah", "Carlos", "Karen", "Wei",
         "Nancy", "Aisha", "Lisa", "Hiro", "Betty", "Ivan", "Sandra", "Omar",
         "Ashley", "Luca", "Emily"]
LAST = ["Smith", "Johnson", "Williams", "Brown", "Jones", "Garcia", "Miller",
        "Davis", "Rodriguez", "Martinez", "Hernandez", "Lopez", "Gonzalez",
        "Wilson", "Anderson", "Thomas", "Taylor", "Moore", "Jackson", "Martin",
        "Lee", "Perez", "Thompson", "White", "Harris", "Sanchez", "Clark",
        "Ramirez", "Lewis", "Robinson", "Walker", "Young"]
PLAIN = ["ok", "pending review", "shipped", "backordered", "returned",
         "gift wrap", "priority", "standard"]

MASK = (1 << 64) - 1


def rows(target):
    st = 0x9E3779B97F4A7C15
    out = ["id,name,qty,price,note\n"]
    size = len(out[0])
    n = 0
    qty_sum = 0
    while size < target:
        vals = []
        for _ in range(6):
            st ^= (st << 13) & MASK
            st ^= st >> 7
            st ^= (st << 17) & MASK
            vals.append(st)
        n += 1
        f, l = FIRST[vals[0] % 32], LAST[(vals[0] >> 8) % 32]
        name = f'"{l}, {f}"' if vals[1] % 4 == 0 else f"{f} {l}"
        qty = vals[2] % 10000
        cents = vals[3] % 1_000_000
        cents = cents - cents % 10 + 1 + (vals[3] >> 20) % 9
        price = f"{cents // 100}.{cents % 100:02d}"
        k = vals[4] % 8
        if k == 0:
            note = '"said ""hi"", then left"'
        elif k == 1:
            note = '"fragile, handle with care"'
        else:
            note = PLAIN[vals[5] % 8]
        line = f"{n},{name},{qty},{price},{note}\n"
        out.append(line)
        size += len(line)
        qty_sum += qty
    return "".join(out).encode(), n, qty_sum


def main():
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    d = sys.argv[1]
    scale = sys.argv[2] if len(sys.argv) > 2 else "full"
    if scale not in TARGET:
        sys.stderr.write(f"gen_csv: scale must be full or smoke, got {scale}\n")
        return 2
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, f"codec-csv-{scale}.csv")
    meta = path[:-4] + ".meta"
    want = SHA256[scale]
    if want and os.path.exists(path) and os.path.exists(meta):
        with open(path, "rb") as fh:
            if hashlib.sha256(fh.read()).hexdigest() == want:
                print(f"[cached]   {os.path.basename(path)}")
                return 0
    data, n, qsum = rows(TARGET[scale])
    have = hashlib.sha256(data).hexdigest()
    if want and have != want:
        sys.stderr.write(f"gen_csv: CHECKSUM MISMATCH for {scale}: expected {want} got {have}\n")
        return 1
    with open(path, "wb") as fh:
        fh.write(data)
    with open(meta, "w") as fh:
        fh.write(f"{n} {qsum} {len(data)}\n")
    print(f"[ok]       {os.path.basename(path)} {len(data)} bytes, {n} rows, sha256 {have}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
