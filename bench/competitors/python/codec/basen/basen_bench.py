#!/usr/bin/env python3
"""codec/base64-{encode,decode} and codec/base32-{encode,decode}: stdlib base64
over 1 MiB of data[i] = (i*31+7) & 0xFF (bench/codec/Base64.md, Base32.md)."""
import base64
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _rows import env, measure, out, row  # noqa: E402

N = 1048576


def main():
    warmup = int(env("PROFILE_WARMUP", "3"))
    trials = int(env("PROFILE_TRIALS", "1000"))   # the page's "1e3 times"
    data = bytes(((i * 31 + 7) & 0xFF) for i in range(N))
    ver = sys.version.split()[0]
    cases = [
        ("base64-encode", base64.b64encode, None, 1398104),
        ("base64-decode", base64.b64decode, base64.b64encode(data), None),
        ("base32-encode", base64.b32encode, None, 1677728),
        ("base32-decode", base64.b32decode, base64.b32encode(data), None),
    ]
    for slug, fn, enc, want_len in cases:
        if enc is None:
            st, res = measure(lambda: fn(data), warmup, trials)
            ok = len(res) == want_len
            note = f"len={len(res)}"
        else:
            st, res = measure(lambda: fn(enc), warmup, trials)
            ok = res == data
            note = "decoded bytes equal the input" if ok else "decoded bytes differ"
        out(row(slug, "", "", N, "base64", ver, warmup, trials, st, ok, note, ""))


if __name__ == "__main__":
    main()
