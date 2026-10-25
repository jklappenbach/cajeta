#!/usr/bin/env python3
"""gen_io.py <dir> <full|smoke> — the io/ benchmarks' generated inputs.

Byte-identical to bench/src/main/cajeta/bench/io/IoData.cajeta (whichever
runs first creates them; a `<name>.done` marker beside each says it is
complete):
  io-seq-<scale>.bin    1 GiB (64 MiB smoke) of one repeated 1 MiB chunk,
                        chunk[j] = ((j >> 12) * 31 + j) & 0xFF
  io-lines-<scale>.txt  1e7 (1e6 smoke) 50-byte lines: the line number as
                        10 zero-padded digits, 39 x chr(97 + k % 26), '\\n'
  io-stat.bin           4096 zero bytes
  tree-<scale>/         depth 3 (2 smoke): 10 subdirectories d0..d9 and 5
                        empty files f0..f4 in every directory
"""
import os
import sys


def done(p):
    return os.path.exists(p + ".done")


def mark(p):
    with open(p + ".done", "wb") as f:
        f.write(b"\n")


def main():
    d, scale = sys.argv[1], sys.argv[2]
    smoke = scale == "smoke"
    os.makedirs(d, exist_ok=True)

    seq = os.path.join(d, f"io-seq-{scale}.bin")
    if not done(seq):
        chunk = bytes((((j >> 12) * 31 + j) & 0xFF) for j in range(1 << 20))
        with open(seq, "wb") as f:
            for _ in range(64 if smoke else 1024):
                f.write(chunk)
            f.flush()
            os.fsync(f.fileno())
        mark(seq)

    lines = os.path.join(d, f"io-lines-{scale}.txt")
    if not done(lines):
        n = 1_000_000 if smoke else 10_000_000
        with open(lines, "wb") as f:
            buf = []
            for k in range(n):
                buf.append(b"%010d%s\n" % (k, bytes([97 + k % 26]) * 39))
                if len(buf) == 20000:
                    f.write(b"".join(buf))
                    buf = []
            f.write(b"".join(buf))
        mark(lines)

    st = os.path.join(d, "io-stat.bin")
    if not done(st):
        with open(st, "wb") as f:
            f.write(bytes(4096))
        mark(st)

    root = os.path.join(d, f"tree-{scale}")
    if not done(root):
        def fill(p, depth):
            os.makedirs(p, exist_ok=True)
            for i in range(5):
                open(os.path.join(p, f"f{i}"), "wb").close()
            if depth:
                for i in range(10):
                    fill(os.path.join(p, f"d{i}"), depth - 1)
        fill(root, 2 if smoke else 3)
        mark(root)


if __name__ == "__main__":
    main()
