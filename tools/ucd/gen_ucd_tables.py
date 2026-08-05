#!/usr/bin/env python3
# gen_ucd_tables.py — generates runtime/native/cajeta_rt_ucd_tables.h from
# the Unicode Character Database (stdlib-completion plan Unit 7; spec §7,
# §8.4: the FULL UCD ships compiled in, so normalization is identical on
# every platform).
#
# Unicode version: 16.0.0, PINNED — asserted against the data files below.
# Regenerate:
#   mkdir ucd && cd ucd
#   for f in UnicodeData.txt CompositionExclusions.txt CaseFolding.txt \
#            DerivedNormalizationProps.txt DerivedCoreProperties.txt \
#            Scripts.txt ArabicShaping.txt extracted/DerivedBidiClass.txt; do
#     curl --create-dirs -o $f https://www.unicode.org/Public/16.0.0/ucd/$f
#   done
#   python3 tools/ucd/gen_ucd_tables.py <ucd-dir> runtime/native/cajeta_rt_ucd_tables.h
#
# Encoding: per-attribute two-stage tries (128-wide blocks, deduped), plus
# flat pools for decompositions and case foldings and a sorted pair table
# for canonical composition. Hangul is algorithmic and appears in NO table.

import sys
from collections import defaultdict

UNICODE_VERSION = "16.0.0"
MAX_CP = 0x110000
BLOCK = 128

SBASE, LBASE, VBASE, TBASE = 0xAC00, 0x1100, 0x1161, 0x11A7
LCOUNT, VCOUNT, TCOUNT = 19, 21, 28
SCOUNT = LCOUNT * VCOUNT * TCOUNT  # 11172


def parse_ranges(path, wanted_field=None):
    """Yield (lo, hi, value) from a UCD semicolon file (cp or cp..cp ; v)."""
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = [p.strip() for p in line.split(";")]
            cps = parts[0]
            val = parts[1] if len(parts) > 1 else ""
            if wanted_field is not None:
                if len(parts) <= wanted_field:
                    continue
                val = parts[wanted_field]
            if ".." in cps:
                lo, hi = (int(x, 16) for x in cps.split(".."))
            else:
                lo = hi = int(cps, 16)
            yield lo, hi, val


def assert_version(path):
    with open(path, encoding="utf-8") as f:
        head = f.readline()
    if UNICODE_VERSION not in head:
        raise SystemExit(f"version pin violated: {path} header is {head.strip()!r}, "
                         f"expected Unicode {UNICODE_VERSION}")


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: gen_ucd_tables.py <ucd-dir> <out-header>")
    ucd, out_path = sys.argv[1], sys.argv[2]

    # Version pin — UnicodeData.txt has no header; these do.
    for f in ("DerivedNormalizationProps.txt", "DerivedCoreProperties.txt",
              "Scripts.txt", "ArabicShaping.txt", "CaseFolding.txt",
              "CompositionExclusions.txt"):
        assert_version(f"{ucd}/{f}")

    # ---- UnicodeData.txt: ccc, canonical/compat decompositions, gc --------
    ccc = {}
    canon = {}     # cp -> [cp...] (unexpanded)
    compat = {}    # cp -> [cp...] (unexpanded, includes canonical)
    gc = {}
    with open(f"{ucd}/UnicodeData.txt", encoding="utf-8") as f:
        prev_first = None
        for line in f:
            p = line.rstrip("\n").split(";")
            cp = int(p[0], 16)
            name = p[1]
            if name.endswith(", First>"):
                prev_first = (cp, p)
                continue
            if name.endswith(", Last>"):
                lo, fp = prev_first
                for c in range(lo, cp + 1):
                    gc[c] = fp[2]
                    if fp[3] and int(fp[3]):
                        ccc[c] = int(fp[3])
                prev_first = None
                continue
            gc[cp] = p[2]
            if p[3] and int(p[3]):
                ccc[cp] = int(p[3])
            d = p[5]
            if d:
                if d.startswith("<"):
                    seq = [int(x, 16) for x in d.split(">", 1)[1].split()]
                    compat[cp] = seq
                else:
                    seq = [int(x, 16) for x in d.split()]
                    canon[cp] = seq
                    compat[cp] = seq

    # ---- recursively expand decompositions (generation-time, not runtime) --
    def expand(mapping, cp, out):
        seq = mapping.get(cp)
        if seq is None:
            out.append(cp)
            return
        for c in seq:
            expand(mapping, c, out)

    canon_full = {}
    for cp in canon:
        out = []
        expand(canon, cp, out)
        canon_full[cp] = out
    compat_full = {}
    for cp in compat:
        out = []
        expand(compat, cp, out)
        compat_full[cp] = out
    # Drop compat entries identical to canonical ones: the runtime falls
    # back K -> canonical -> identity, so only differences need storage.
    compat_only = {cp: seq for cp, seq in compat_full.items()
                   if canon_full.get(cp) != seq}

    # ---- DerivedNormalizationProps: QC + Full_Composition_Exclusion --------
    qc = defaultdict(int)  # 2 bits per form: bits[0:2]=NFD, [2:4]=NFC, [4:6]=NFKD, [6:8]=NFKC
    FORM_SHIFT = {"NFD_QC": 0, "NFC_QC": 2, "NFKD_QC": 4, "NFKC_QC": 6}
    VAL = {"N": 2, "M": 1}
    full_excl = set()
    with open(f"{ucd}/DerivedNormalizationProps.txt", encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = [p.strip() for p in line.split(";")]
            if len(parts) < 2:
                continue
            cps, prop = parts[0], parts[1]
            lo, hi = (int(x, 16) for x in cps.split("..")) if ".." in cps \
                else (int(cps, 16),) * 2
            if prop == "Full_Composition_Exclusion":
                full_excl.update(range(lo, hi + 1))
            elif prop in FORM_SHIFT and len(parts) >= 3 and parts[2] in VAL:
                for c in range(lo, hi + 1):
                    qc[c] |= VAL[parts[2]] << FORM_SHIFT[prop]

    # ---- canonical composition pairs --------------------------------------
    pairs = {}
    for cp, seq in canon.items():           # UNEXPANDED pairs, per UAX #15
        if len(seq) == 2 and cp not in full_excl:
            pairs[(seq[0], seq[1])] = cp

    # ---- CaseFolding.txt: full folding (C + F) -----------------------------
    fold = {}
    for lo, hi, _ in []:
        pass
    with open(f"{ucd}/CaseFolding.txt", encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            p = [x.strip() for x in line.split(";")]
            if len(p) < 3 or p[1] not in ("C", "F"):
                continue
            fold[int(p[0], 16)] = [int(x, 16) for x in p[2].split()]

    # ---- DerivedCoreProperties: Default_Ignorable_Code_Point ---------------
    ignorable = set()
    for lo, hi, val in parse_ranges(f"{ucd}/DerivedCoreProperties.txt"):
        if val == "Default_Ignorable_Code_Point":
            ignorable.update(range(lo, hi + 1))

    # ---- Scripts.txt -------------------------------------------------------
    script_ids = {"Unknown": 0}
    script = {}
    for lo, hi, val in parse_ranges(f"{ucd}/Scripts.txt"):
        sid = script_ids.setdefault(val, len(script_ids))
        for c in range(lo, hi + 1):
            script[c] = sid

    # ---- ArabicShaping.txt: joining type; default T for Mn/Me/Cf, else U ---
    JOIN = {"U": 0, "T": 1, "R": 2, "L": 3, "D": 4, "C": 5}
    joining = {}
    with open(f"{ucd}/ArabicShaping.txt", encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            p = [x.strip() for x in line.split(";")]
            if len(p) < 3:
                continue
            joining[int(p[0], 16)] = JOIN[p[2]]
    for cp, g in gc.items():
        if cp not in joining and g in ("Mn", "Me", "Cf"):
            joining[cp] = JOIN["T"]

    # ---- extracted/DerivedBidiClass.txt ------------------------------------
    BIDI = {}
    bidi = {}
    for lo, hi, val in parse_ranges(f"{ucd}/extracted/DerivedBidiClass.txt"):
        bid = BIDI.setdefault(val, len(BIDI))
        for c in range(lo, hi + 1):
            bidi[c] = bid

    # ---- pools -------------------------------------------------------------
    # Decomposition pool: uint32 entries; record = [len | (kindbits)] then cps.
    # Index 0 reserved for "none". Two maps share the pool.
    pool = [0]
    def pool_add(seq):
        idx = len(pool)
        pool.append(len(seq))
        pool.extend(seq)
        return idx

    canon_idx = {cp: pool_add(seq) for cp, seq in sorted(canon_full.items())}
    compat_idx = {cp: pool_add(seq) for cp, seq in sorted(compat_only.items())}
    fold_idx = {cp: pool_add(seq) for cp, seq in sorted(fold.items())}

    # ---- two-stage trie builder -------------------------------------------
    def build_trie(get, default, fmt):
        stage1 = []
        blocks = {}
        stage2 = []
        for b in range(0, MAX_CP, BLOCK):
            block = tuple(get(c, default) for c in range(b, b + BLOCK))
            if block not in blocks:
                blocks[block] = len(stage2) // BLOCK
                stage2.extend(block)
            stage1.append(blocks[block])
        return stage1, stage2, fmt

    tries = {
        "ccc": build_trie(ccc.get, 0, "uint8_t"),
        "qc": build_trie(qc.get, 0, "uint8_t"),
        "canon": build_trie(canon_idx.get, 0, "uint32_t"),
        "compat": build_trie(compat_idx.get, 0, "uint32_t"),
        "fold": build_trie(fold_idx.get, 0, "uint32_t"),
        "ign": build_trie(lambda c, d: 1 if c in ignorable else 0, 0, "uint8_t"),
        "script": build_trie(script.get, 0, "uint16_t"),
        "join": build_trie(joining.get, 0, "uint8_t"),
        "bidi": build_trie(bidi.get, BIDI.get("L", 0), "uint8_t"),
    }

    pair_rows = sorted(((a << 21) | b, c) for (a, b), c in pairs.items())

    # ---- emit --------------------------------------------------------------
    total_bytes = 0
    def arr_bytes(vals, fmt):
        w = {"uint8_t": 1, "uint16_t": 2, "uint32_t": 4, "uint64_t": 8}[fmt]
        return len(vals) * w

    with open(out_path, "w", encoding="utf-8") as o:
        o.write("// AUTO-GENERATED by tools/ucd/gen_ucd_tables.py — DO NOT EDIT.\n")
        o.write(f"// Unicode Character Database {UNICODE_VERSION} "
                "(stdlib-completion U7, spec §7/§8.4).\n")
        o.write("// Two-stage tries: value = stage2[stage1[cp >> 7] * 128 + (cp & 127)].\n\n")
        o.write("#include <stdint.h>\n\n")
        o.write(f"#define CAJ_UCD_VERSION \"{UNICODE_VERSION}\"\n")
        o.write(f"#define CAJ_UCD_BLOCK_SHIFT 7\n#define CAJ_UCD_BLOCK_MASK 127\n\n")

        def emit(name, vals, fmt):
            nonlocal total_bytes
            total_bytes += arr_bytes(vals, fmt)
            o.write(f"static const {fmt} {name}[{len(vals)}] = {{\n")
            for i in range(0, len(vals), 16):
                o.write("  " + ",".join(str(v) for v in vals[i:i+16]) + ",\n")
            o.write("};\n\n")

        for key, (s1, s2, fmt) in tries.items():
            s1fmt = "uint16_t" if max(s1) < 65536 else "uint32_t"
            emit(f"caj_ucd_{key}_stage1", s1, s1fmt)
            emit(f"caj_ucd_{key}_stage2", s2, fmt)

        emit("caj_ucd_pool", pool, "uint32_t")
        emit("caj_ucd_pair_keys", [k for k, _ in pair_rows], "uint64_t")
        emit("caj_ucd_pair_vals", [v for _, v in pair_rows], "uint32_t")
        o.write(f"#define CAJ_UCD_PAIR_COUNT {len(pair_rows)}\n")

        names = sorted(script_ids, key=script_ids.get)
        o.write(f"static const char* const caj_ucd_script_names[{len(names)}] = {{\n")
        for n in names:
            o.write(f'  "{n}",\n')
        o.write("};\n")
        o.write(f"#define CAJ_UCD_SCRIPT_COUNT {len(names)}\n")
        bnames = sorted(BIDI, key=BIDI.get)
        o.write(f"static const char* const caj_ucd_bidi_names[{len(bnames)}] = {{\n")
        for n in bnames:
            o.write(f'  "{n}",\n')
        o.write("};\n")
        o.write(f"#define CAJ_UCD_BIDI_COUNT {len(bnames)}\n")
        o.write(f"\n// total table bytes: {total_bytes}\n")

    print(f"wrote {out_path}: {total_bytes} table bytes, "
          f"{len(pair_rows)} composition pairs, "
          f"{len(canon_idx)} canonical / {len(compat_idx)} compat-only "
          f"decompositions, {len(fold_idx)} foldings, "
          f"{len(names)} scripts, {len(bnames)} bidi classes")


if __name__ == "__main__":
    main()
