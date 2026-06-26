#!/usr/bin/env python3
"""profile report generator (Unit 16).

Reads the raw benchmark CSVs (results.csv + env.csv) and emits, from the data
ONLY (it never runs a benchmark):

  * report.md       — grouped Markdown comparison tables
  * site/index.html — a self-contained, *cajeta*-themed (caramel / dulce-de-leche
                      tans + deep browns, black pinlines) dashboard: a left-hand
                      index of every benchmark, and a detail panel per benchmark
                      with Time / Throughput / Memory tabs, each a vertical bar
                      chart (one bar per competitor, taller = better, the actual
                      value + unit labelled). Reference-machine env block + a
                      light/dark toggle. No external assets.

Interim reporter: once Cajeta has a dataframe/plotting capability a Cajeta-native
reporter replaces this, consuming the same CSV. Pure stdlib (csv) — no deps.

Usage: report.py <results-dir>        # reads <dir>/results.csv + <dir>/env.csv,
                                       # writes <dir>/report.md + <dir>/site/
"""
import csv
import html
import os
import re
import sys

AREAS_ORDER = ["codec", "collection", "sort", "string", "hash", "stream",
               "math", "clbg", "time", "concurrent", "xpu"]

# Per-language bar colours — all in the cajeta caramel family, distinguishable.
LANG_COLORS = {
    "cajeta": "#5C3310",   # deepest — the star
    "rust":   "#C77B3A",
    "cpp":    "#9A5A24",
    "go":     "#E0B36A",
    "python": "#7E5430",
    "java":   "#B08855",
}
DEFAULT_COLOR = "#A0744A"


def read_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def read_env(path):
    env = {}
    if os.path.exists(path):
        with open(path, newline="") as f:
            for row in csv.reader(f):
                if len(row) >= 2 and row[0] != "key":
                    env[row[0]] = row[1]
    return env


def num(v):
    try:
        return int(v)
    except (ValueError, TypeError):
        return None


def fnum(v):
    try:
        return float(v)
    except (ValueError, TypeError):
        return None


def ms(ns):
    n = num(ns)
    return "" if n is None or n < 0 else f"{n / 1e6:.3f}"


def rate(size, ns):
    s, n = num(size), num(ns)
    if s is None or n is None or n <= 0 or s <= 0:
        return ""
    r = s * 1e9 / n
    for unit, div in (("G", 1e9), ("M", 1e6), ("K", 1e3)):
        if r >= div:
            return f"{r / div:.2f}{unit}/s"
    return f"{r:.0f}/s"


def fmt(v):
    if v >= 1000:
        return f"{v:,.0f}"
    if v >= 100:
        return f"{v:.0f}"
    if v >= 10:
        return f"{v:.1f}"
    if v >= 1:
        return f"{v:.2f}"
    return f"{v:.3f}"


def slug(s):
    return re.sub(r"[^a-z0-9]+", "-", s.lower()).strip("-") or "x"


def lang_color(lang):
    return LANG_COLORS.get(lang, DEFAULT_COLOR)


def clean_version(lang, ver):
    """Trim a verbose `--version` string down to the meaningful version for the
    legend table (the Language column already names the language). Keeps cajeta's
    commit hash and the C++ compiler name; everything else collapses to N.N.N."""
    ver = ver.strip()
    if lang == "cajeta":
        return ver[len("cajeta "):] if ver.startswith("cajeta ") else ver
    m = re.search(r"\d+\.\d+(?:\.\d+)?", ver)
    num = m.group(0) if m else ver
    if lang == "cpp":
        low = ver.lower()
        comp = "clang" if "clang" in low else ("g++" if "g++" in low or "gcc" in low else "")
        return (comp + " " + num).strip()
    return num


def lang_versions(rows, env):
    """Map each language -> its (cleaned) toolchain version. Competitor versions
    are carried per-row in `language_version` (each runner records its own
    rustc/go/javac/python/g++ --version); cajeta's is stamped into env.csv as
    `cajeta_version` (the Cajeta side emits no version column of its own)."""
    out = {}
    for r in rows:
        la = r.get("language", "")
        ver = (r.get("language_version") or "").strip()
        if la and ver and la not in out:
            out[la] = clean_version(la, ver)
    cv = (env.get("cajeta_version") or "").strip()
    if cv:
        out["cajeta"] = clean_version("cajeta", cv)
    return out


def row_label(r):
    lang = r.get("language", "")
    lib = r.get("library", "")
    return lang if (not lib or lib == lang) else f"{lang}/{lib}"


def group_by_area(rows):
    g = {}
    for r in rows:
        area = r.get("area", "")
        if area in ("scaffold", "selftest", "none", ""):
            continue
        g.setdefault(area, []).append(r)
    order = AREAS_ORDER + [a for a in g if a not in AREAS_ORDER]
    return [(a, g[a]) for a in order if a in g]


# Datasets are encoded two ways: competitors set dataset_name; the Cajeta
# benchmarks instead put the dataset in the variant column (variant=twitter/citm/
# canada) or distinguish only by input_size. So "dataset" is unified below, and a
# variant that merely names a known dataset is NOT treated as a real variant (it
# becomes the in-panel dataset <select>, not a separate sidebar entry).

def area_dataset_names(rows):
    d = {}
    for r in rows:
        dn = r.get("dataset_name", "")
        if dn:
            d.setdefault(r.get("area", ""), set()).add(dn)
    return d


def area_size_to_ds(rows):
    d = {}
    for r in rows:
        dn = r.get("dataset_name", "")
        if dn:
            d.setdefault(r.get("area", ""), {})[r.get("input_size", "")] = dn
    return d


def real_variant(r, area_ds):
    v = r.get("variant", "")
    if v and v in area_ds.get(r.get("area", ""), ()):
        return ""  # a dataset-as-variant is not a real variant
    return v


def row_dataset(r, area_ds, area_s2d):
    dn = r.get("dataset_name", "")
    if dn:
        return dn
    a = r.get("area", "")
    v = r.get("variant", "")
    if v and v in area_ds.get(a, ()):
        return v
    return area_s2d.get(a, {}).get(r.get("input_size", ""), "")


def benchmarks(rows):
    """Ordered list of (area, benchmark, variant, key, rows) — one per distinct
    (area, benchmark, real-variant) comparison, AREAS_ORDER then first-seen order.
    Dataset-as-variant rows collapse into the same benchmark (selected in-panel)."""
    area_ds = area_dataset_names(rows)
    order, g = [], {}
    for r in rows:
        area = r.get("area", "")
        if area in ("scaffold", "selftest", "none", ""):
            continue
        k = (area, r.get("benchmark", ""), real_variant(r, area_ds))
        if k not in g:
            g[k] = []
            order.append(k)
        g[k].append(r)

    def akey(k):
        return (AREAS_ORDER.index(k[0]) if k[0] in AREAS_ORDER else 99, order.index(k))

    out = []
    for k in sorted(order, key=akey):
        area, bench, variant = k
        out.append((area, bench, variant, slug("-".join(k)), g[k]))
    return out


# ----------------------------------------------------------------- series ----
# Each metric tab is a "series": a unit, a better-direction, and a list of bars.
# Bars are normalised so taller is ALWAYS better (the actual value is labelled),
# which keeps Time/Throughput/Memory visually consistent.

def measured(rows):
    return [r for r in rows if r.get("status") == "ok"]


def time_series(rows):
    vals = [(r, fnum(r.get("median_ns"))) for r in measured(rows)]
    vals = [(r, v) for r, v in vals if v and v > 0]
    if not vals:
        return None
    best = min(v for _, v in vals)
    if best >= 1e9:
        div, unit = 1e9, "s"
    elif best >= 1e6:
        div, unit = 1e6, "ms"
    elif best >= 1e3:
        div, unit = 1e3, "µs"
    else:
        div, unit = 1.0, "ns"
    bars = [dict(r=r, value=v / div, score=best / v) for r, v in vals]
    return dict(unit=unit, better="lower", bars=bars)


# Cajeta benchmarks don't emit a throughput column — only median_ns + input_size.
# Every competitor throughput unit except math's GFLOP/s is just input_size/time,
# so we derive Cajeta's value in the chart's own unit (the same formula the
# competitor runners use) — keeping Cajeta first-class in the Throughput tab.
THR_DIV = {
    "B/s": 1, "KB/s": 1024, "MB/s": 1048576, "GB/s": 1073741824,
    "op/s": 1, "Kop/s": 1e3, "Mop/s": 1e6, "Gop/s": 1e9,
    "elem/s": 1, "Kelem/s": 1e3, "Melem/s": 1e6, "Gelem/s": 1e9,
}
# When no row carries an explicit unit, infer one from the area's input_size
# meaning (bytes vs element/op count). math (GFLOP/s) and clbg (time) → no derive.
AREA_THR_UNIT = {
    "codec": "MB/s", "hash": "MB/s", "string": "MB/s",
    "sort": "Melem/s",
    "collection": "Mop/s", "stream": "Mop/s", "time": "Mop/s", "concurrent": "Mop/s",
    "math": "GFLOP/s",
}


# GFLOP/s can't come from a size/time divisor — it needs the benchmark's FLOP
# count. These mirror the competitor runners' exact conventions (math_bench.py /
# main.go / main.rs / math_bench.cpp): input_size is the element count, and
#   saxpy / dot-product : 2·N      (N = input_size)
#   matmul              : 2·N³     (input_size = N², so N³ = input_size ** 1.5)
# so Cajeta — which emits only median_ns + input_size — stays first-class on the
# math Throughput tab instead of silently dropping out of the default view.
def math_flops(bench, size):
    if not size or size <= 0:
        return None
    if bench == "matmul":
        return 2.0 * size ** 1.5
    if bench in ("dot-product", "saxpy"):
        return 2.0 * size
    return None


def derive_thr(unit, size, ns, bench=None):
    if not ns or ns <= 0:
        return None
    if unit == "GFLOP/s":
        f = math_flops(bench, size)
        return f / ns if f else None      # FLOP per ns == GFLOP/s
    d = THR_DIV.get(unit)
    if d is None or not size or size <= 0:
        return None
    return size / ns * 1e9 / d


def thr_series(rows, area):
    unit = next((r.get("throughput_unit", "") for r in measured(rows)
                 if fnum(r.get("throughput")) and r.get("throughput_unit")), None)
    unit = unit or AREA_THR_UNIT.get(area)
    if not unit:
        return None
    vals = []
    for r in measured(rows):
        t = fnum(r.get("throughput"))
        if not (t and t > 0):
            t = derive_thr(unit, fnum(r.get("input_size")), fnum(r.get("median_ns")),
                           r.get("benchmark"))
        if t and t > 0:
            vals.append((r, t))
    if not vals:
        return None
    mx = max(v for _, v in vals)
    bars = [dict(r=r, value=v, score=v / mx) for r, v in vals]
    return dict(unit=unit, better="higher", bars=bars)


def mem_series(rows):
    # alloc_bytes = bytes the benchmark asked the allocator for (one execution) —
    # the runtime-independent, comparable memory metric (tracemalloc / Go MemStats
    # / a Rust counting global allocator / a C++ operator-new counter). RSS-based
    # numbers are NOT used: they measure different things per runtime (an allocator
    # that retains vs one that returns), so they aren't comparable. Rows without
    # alloc_bytes (e.g. the Cajeta harness, which doesn't emit it) become n/a.
    vals = [(r, fnum(r.get("alloc_bytes"))) for r in measured(rows)]
    vals = [(r, v) for r, v in vals if v is not None and v >= 0]
    if not vals:
        return None
    pos = [v for _, v in vals if v > 0]
    base = min(pos) if pos else 1.0
    big = max((v for _, v in vals), default=0)
    if big >= 1 << 30:
        div, unit = float(1 << 30), "GB"
    elif big >= 1 << 20:
        div, unit = float(1 << 20), "MB"
    elif big >= 1 << 10:
        div, unit = float(1 << 10), "KB"
    else:
        div, unit = 1.0, "B"
    # lower is better; allocating nothing (0) is the leanest → tallest bar.
    bars = [dict(r=r, value=v / div, score=(base / v if v > 0 else 1.0)) for r, v in vals]
    return dict(unit=unit, better="lower", bars=bars)


# ---------------------------------------------------------------- Markdown ----
def gen_markdown(rows, env):
    out = ["# Cajeta profile — benchmark report", ""]
    if env:
        out.append("Reference machine: " + " · ".join(
            f"**{k}** {v}" for k, v in env.items()
            if k not in ("run_id", "cajeta_version")) + "\n")
    # Languages legend — toolchain versions behind each competitor.
    lv = lang_versions(rows, env)
    langs_present = []
    for r in rows:
        la = r.get("language", "")
        if la and la not in langs_present:
            langs_present.append(la)
    if langs_present:
        out.append("## Languages\n")
        out.append("| Language | Version |")
        out.append("|---|---|")
        for la in langs_present:
            tag = " ★" if la == "cajeta" else ""
            out.append(f"| {la}{tag} | {lv.get(la) or '—'} |")
        out.append("")
    for area, rs in group_by_area(rows):
        out.append(f"## {area}")
        out.append("")
        out.append("| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |")
        out.append("|---|---|---|---|---|---|---|---|---|---|")
        for r in rs:
            out.append("| {b} | {v} | {l} | {lib} | {i} | {mn} | {md} | {rt} | {ws} | {st} |".format(
                b=r.get("benchmark", ""), v=r.get("variant", ""), l=r.get("language", ""),
                lib=r.get("library", "") or "stdlib",
                i=r.get("input_size", ""), mn=ms(r.get("min_ns")), md=ms(r.get("median_ns")),
                rt=rate(r.get("input_size"), r.get("min_ns")),
                ws=r.get("working_set_kb", ""), st=r.get("status", "")))
        out.append("")
    return "\n".join(out)


# -------------------------------------------------------------------- HTML ----
# cajeta = the caramel confection. Palette is tans + deep caramel browns; every
# surface gets a black pinline. Light is the primary look; dark stays caramel.
CSS = r"""
:root{
  --bg:#EFE0C6; --panel:#FBF4E6; --panel2:#F3E7CF; --fg:#2C1B0C; --muted:#7A5C36;
  --tan:#E7CFA6; --caramel:#B06A2C; --deep:#7A481E; --espresso:#3A2412;
  --gold:#D49A3C; --line:#1A0E04; --hair:rgba(122,72,30,.28); --shadow:rgba(58,36,18,.16);
}
:root[data-theme="dark"]{
  --bg:#170E07; --panel:#2A1B10; --panel2:#22150B; --fg:#F3E5CD; --muted:#BC9B6F;
  --tan:#3A2716; --caramel:#C77F38; --deep:#E2B36C; --espresso:#F0DDBE;
  --gold:#E8B655; --line:#000; --hair:rgba(226,179,108,.26); --shadow:rgba(0,0,0,.45);
}
*{box-sizing:border-box}
html,body{margin:0}
body{font:15px/1.55 -apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif;
  background:var(--bg);color:var(--fg);
  background-image:radial-gradient(circle at 18% -10%,rgba(212,154,60,.14),transparent 42%),
                   radial-gradient(circle at 100% 0%,rgba(122,72,30,.12),transparent 38%);}
h1,h2,h3{font-family:Georgia,"Times New Roman",serif}
a{color:inherit}

/* ---- header ---- */
header{position:relative;color:#FBF1DD;padding:22px 30px;
  background:linear-gradient(120deg,var(--espresso),var(--deep) 42%,var(--caramel) 78%,var(--gold));
  border-bottom:2px solid var(--line);}
:root[data-theme="dark"] header{color:#1c1208}
header h1{margin:0;font-size:27px;letter-spacing:.3px;text-shadow:0 1px 0 rgba(0,0,0,.25)}
header .sub{opacity:.94;margin-top:3px;font-size:14px}
.toggle{position:absolute;top:18px;right:24px;background:rgba(0,0,0,.18);
  border:1px solid rgba(255,255,255,.35);color:inherit;border-radius:999px;padding:6px 14px;
  cursor:pointer;font-size:13px;font-weight:600}
.toggle:hover{background:rgba(0,0,0,.3)}

/* ---- layout ---- */
.app{display:flex;align-items:flex-start;gap:0}
.sidebar{position:sticky;top:0;align-self:flex-start;width:268px;flex:0 0 268px;
  max-height:100vh;overflow:auto;padding:16px 12px 40px;
  background:var(--panel2);border-right:1px solid var(--hair)}
.main{flex:1 1 auto;min-width:0;padding:22px 26px 60px;max-width:1080px}

/* ---- sidebar index (no per-item pinlines — highlight by fill + gold rail) ---- */
.sb-area{font-family:Georgia,serif;font-size:12px;letter-spacing:.14em;text-transform:uppercase;
  color:var(--caramel);margin:16px 8px 6px;font-weight:700}
.navbench{display:flex;justify-content:space-between;gap:8px;align-items:center;
  padding:7px 10px;margin:2px 2px;border-radius:8px;cursor:pointer;font-size:13.5px;color:var(--fg)}
.navbench:hover{background:var(--panel)}
.navbench.active{background:linear-gradient(90deg,var(--tan),var(--panel));
  box-shadow:inset 3px 0 0 var(--gold);font-weight:600}
.navbench .vtag{font-size:10.5px;color:var(--muted);white-space:nowrap}

/* ---- env strip (soft hairline only) ---- */
.env{display:flex;flex-wrap:wrap;gap:6px 16px;background:var(--panel);
  border:1px solid var(--hair);border-radius:12px;padding:12px 18px;margin:0 0 18px}
.env span{font-size:12.5px}
.env b{color:var(--caramel);font-weight:700}

/* ---- legend ---- */
.legend{display:flex;flex-wrap:wrap;gap:10px 16px;margin:0 0 14px;font-size:12.5px}
.legend .sw{display:inline-flex;align-items:center;gap:6px}
.legend .chip{width:14px;height:14px;border:1px solid var(--hair);border-radius:3px;display:inline-block}

/* ---- language + version legend table ---- */
.langtable{border-collapse:collapse;margin:0 0 22px;font-size:12.5px;
  border:1px solid var(--hair);border-radius:10px;overflow:hidden}
.langtable th,.langtable td{text-align:left;padding:5px 14px;border-bottom:1px solid var(--hair)}
.langtable thead th{background:var(--panel);color:var(--caramel);font-weight:700}
.langtable tbody tr:last-child td{border-bottom:none}
.langtable .chip{width:12px;height:12px;border:1px solid var(--hair);border-radius:3px;
  display:inline-block;vertical-align:middle;margin-right:7px}
.langtable td.ver{color:var(--ink-soft, #6b5a48);font-variant-numeric:tabular-nums}

/* ---- panels (the one place that keeps a true black pinline) ---- */
.panel{display:none;background:var(--panel);border:2px solid var(--line);border-radius:14px;
  padding:20px 22px 24px;box-shadow:4px 4px 0 var(--shadow);margin-bottom:24px}
.panel.show{display:block}
.panel h2{margin:0 0 2px;font-size:22px;color:var(--deep)}
:root[data-theme="dark"] .panel h2{color:var(--gold)}
.panel .meta{color:var(--muted);font-size:12.5px;margin-bottom:16px}
.panel .meta .pill{display:inline-block;border-radius:999px;padding:1px 10px;margin-right:6px;
  background:var(--tan);color:var(--espresso);font-weight:600}

/* ---- tabs: the active tab OPENS into the content (no line under it); the rest
        sit behind the strip's hairline ---- */
.tabs{display:flex;gap:6px;margin:0 0 16px;border-bottom:2px solid var(--line)}
.tabb{appearance:none;cursor:pointer;font:inherit;font-weight:600;font-size:13px;
  padding:8px 17px;color:var(--muted);background:transparent;
  border:2px solid transparent;border-bottom:none;border-radius:9px 9px 0 0;margin-bottom:-2px}
.tabb:hover{color:var(--fg);background:var(--panel2)}
.tabb.active{color:var(--espresso);background:var(--panel);
  border-color:var(--line);border-top:3px solid var(--gold);
  border-bottom:2px solid var(--panel)}   /* erases the strip line under the active tab */
:root[data-theme="dark"] .tabb.active{color:var(--espresso)}
.tabc{display:none}
.tabc.show{display:block}

/* ---- bar chart ---- */
.axis{font-size:12px;color:var(--muted);margin:2px 2px 12px;font-weight:600}
.axis .dir{color:var(--caramel)}
.chart{display:flex;align-items:flex-end;gap:16px;height:300px;padding:8px 6px 0;
  overflow-x:auto;border-bottom:2px solid var(--line)}
.col{display:flex;flex-direction:column;align-items:center;justify-content:flex-end;
  height:100%;min-width:58px}
.val{font-size:12px;font-weight:700;margin-bottom:5px;white-space:nowrap;color:var(--fg)}
.bar{width:46px;border:1.5px solid var(--line);border-radius:4px 4px 0 0;min-height:6px;
  background-image:linear-gradient(180deg,rgba(255,255,255,.32),rgba(0,0,0,.06));
  box-shadow:2px 2px 0 var(--shadow);transition:transform .08s ease}
.col:hover .bar{transform:translateY(-3px)}
.bar.star{border-width:2.5px;box-shadow:0 0 0 1.5px var(--gold),2px 2px 0 var(--shadow)}
.bar.stub{height:9px!important;border-style:dashed;box-shadow:none;
  background:repeating-linear-gradient(45deg,var(--tan),var(--tan) 5px,var(--panel2) 5px,var(--panel2) 10px)}
/* fixed height so every bar shares one baseline regardless of how many lines the
   library name wraps to (Cajeta has no library line, competitors have long ones) */
.name{font-size:11px;margin-top:8px;text-align:center;width:78px;line-height:1.25;
  height:54px;overflow:hidden;word-break:break-word;color:var(--fg);flex:0 0 auto}
.name .lib{display:block;color:var(--muted);font-size:10px}
.name.cajeta{font-weight:700;color:var(--deep)}
:root[data-theme="dark"] .name.cajeta{color:var(--gold)}
.empty{padding:40px 8px;color:var(--muted);font-style:italic}
.status-skipped{color:#9a6b12;font-weight:600}
:root[data-theme="dark"] .status-skipped{color:var(--gold)}

/* ---- dataset select + variant suffix ---- */
.vsuf{color:var(--muted);font-weight:400;font-size:16px}
.dslabel{display:inline-block;font-size:12.5px;color:var(--muted);margin:0 0 14px;font-weight:600}
.dssel{font:inherit;font-size:13px;font-weight:600;color:var(--espresso);background:var(--tan);
  border:1px solid var(--hair);border-radius:8px;padding:5px 11px;margin-left:6px;cursor:pointer}

footer{color:var(--muted);font-size:12px;padding:26px 30px;text-align:center}
footer code{background:var(--panel);border:1px solid var(--hair);border-radius:4px;padding:0 5px}
"""


def bar_col(b, better, unit):
    r = b["r"]
    lang = r.get("language", "")
    label = row_label(r)
    lib = r.get("library", "")
    color = lang_color(lang)
    h = max(6, round(b["score"] * 210))
    star = " star" if lang == "cajeta" else ""
    nm_cls = " cajeta" if lang == "cajeta" else ""
    star_mark = "★ " if lang == "cajeta" else ""
    libline = f"<span class='lib'>{html.escape(lib)}</span>" if (lib and lib != lang) else ""
    tip = f"{label} — {fmt(b['value'])} {unit}"
    return (
        f"<div class='col' title='{html.escape(tip)}'>"
        f"<div class='val'>{fmt(b['value'])}</div>"
        f"<div class='bar{star}' style='height:{h}px;background-color:{color}'></div>"
        f"<div class='name{nm_cls}'>{star_mark}{html.escape(lang)}{libline}</div>"
        f"</div>"
    )


def stub_col(r):
    label = row_label(r)
    lib = r.get("library", "")
    libline = f"<span class='lib'>{html.escape(lib)}</span>" if (lib and lib != lang_of(r)) else ""
    st = r.get("status", "skipped")
    note = r.get("notes", "")
    tip = f"{label} — {st}" + (f": {note}" if note else "")
    return (
        f"<div class='col' title='{html.escape(tip)}'>"
        f"<div class='val status-skipped'>{html.escape(st)}</div>"
        f"<div class='bar stub'></div>"
        f"<div class='name'>{html.escape(lang_of(r))}{libline}</div>"
        f"</div>"
    )


def lang_of(r):
    return r.get("language", "")


def na_col(r):
    """A measured row that has no value for *this* metric (e.g. competitors lack a
    working-set delta) — shown as an explicit n/a stub, not silently dropped."""
    lib = r.get("library", "")
    libline = f"<span class='lib'>{html.escape(lib)}</span>" if (lib and lib != lang_of(r)) else ""
    return (
        f"<div class='col' title='{html.escape(row_label(r))} — n/a'>"
        f"<div class='val' style='color:var(--muted)'>n/a</div>"
        f"<div class='bar stub'></div>"
        f"<div class='name'>{html.escape(lang_of(r))}{libline}</div>"
        f"</div>"
    )


def render_tab(series, measured_rows, skipped, metric_name):
    have = set(id(b["r"]) for b in (series["bars"] if series else []))
    na = [r for r in measured_rows if id(r) not in have]
    if series is None and not na and not skipped:
        return f"<div class='empty'>No {metric_name} data for this benchmark.</div>"
    bars = sorted(series["bars"], key=lambda b: b["score"], reverse=True) if series else []
    cols = "".join(bar_col(b, series["better"], series["unit"]) for b in bars)
    cols += "".join(na_col(r) for r in na)
    cols += "".join(stub_col(r) for r in skipped)
    if series is None:
        axis = f"<div class='axis'><b>{metric_name}</b> · <span class='dir'>not captured for these runners</span></div>"
    else:
        if series["better"] == "higher":
            arrow = "↑ taller is better"
        elif metric_name == "Memory":
            arrow = "↓ lower is better — taller bar = leaner (bytes allocated, one execution)"
        else:
            arrow = "↓ lower is better — taller bar = faster"
        axis = (f"<div class='axis'><b>{metric_name}</b> · unit <b>{html.escape(series['unit'])}</b> "
                f"· <span class='dir'>{arrow}</span></div>")
    return axis + f"<div class='chart'>{cols}</div>"


def gen_html(rows, env):
    benches = benchmarks(rows)

    # sidebar
    sb = ["<aside class='sidebar'>"]
    cur_area = None
    for area, bench, variant, key, _rs in benches:
        if area != cur_area:
            sb.append(f"<div class='sb-area'>{html.escape(area)}</div>")
            cur_area = area
        vtag = f"<span class='vtag'>{html.escape(variant)}</span>" if variant else ""
        sb.append(f"<div class='navbench' id='nav-{key}' onclick=\"showBench('{key}')\">"
                  f"<span>{html.escape(bench)}</span>{vtag}</div>")
    sb.append("</aside>")

    # main
    main = ["<main class='main'>"]
    if env:
        main.append("<div class='env'>" + "".join(
            f"<span><b>{html.escape(k)}</b> {html.escape(v)}</span>"
            for k, v in env.items() if k not in ("run_id", "cajeta_version")) + "</div>")
    # language legend
    langs_present = []
    for *_x, rs in benches:
        for r in rs:
            la = r.get("language", "")
            if la and la not in langs_present:
                langs_present.append(la)
    main.append("<div class='legend'>" + "".join(
        f"<span class='sw'><span class='chip' style='background:{lang_color(la)}'></span>"
        f"{html.escape(la)}{' ★' if la == 'cajeta' else ''}</span>" for la in langs_present) + "</div>")
    # language + version legend table. Versions come from each competitor
    # runner's `language_version` (rustc/go/javac/python/g++ --version); cajeta's
    # is stamped into env.csv as `cajeta_version` by bench.sh.
    lang_ver = lang_versions(rows, env)
    if langs_present:
        main.append(
            "<table class='langtable'><thead><tr><th>Language</th><th>Version</th>"
            "</tr></thead><tbody>" + "".join(
                f"<tr><td><span class='chip' style='background:{lang_color(la)}'>"
                f"</span>{html.escape(la)}{' ★' if la == 'cajeta' else ''}</td>"
                f"<td class='ver'>{html.escape(lang_ver.get(la) or '—')}</td></tr>"
                for la in langs_present) + "</tbody></table>")

    area_ds = area_dataset_names(rows)
    area_s2d = area_size_to_ds(rows)
    ds_size = {a: {d: s for s, d in m.items()} for a, m in area_s2d.items()}

    metrics = (("thr", "Throughput"), ("time", "Time"), ("mem", "Memory"))
    first = True
    for area, bench, variant, key, rs in benches:
        # A benchmark may run over several datasets (codec: twitter/citm/canada).
        # Dataset is an in-panel <select>, NOT a separate sidebar entry — so each
        # chart compares one dataset cleanly across competitors.
        dsof = lambda r: row_dataset(r, area_ds, area_s2d)
        sizemap = ds_size.get(area, {})
        named = []
        for r in rs:
            d = dsof(r)
            if d and d not in named:
                named.append(d)
        named.sort(key=lambda d: fnum(sizemap.get(d, "")) or 0)
        multi = len(named) > 1

        if multi:
            # rows we still couldn't slot: one stub per language, shown in the
            # datasets where that language is otherwise absent.
            floating, seen = [], set()
            for r in rs:
                if not dsof(r):
                    la = r.get("language", "")
                    if la not in seen:
                        seen.add(la)
                        floating.append(r)
            blocks = []
            for d in named:
                drows = [r for r in rs if dsof(r) == d]
                here = {r.get("language", "") for r in drows}
                drows += [r for r in floating if r.get("language", "") not in here]
                blocks.append((d, drows))
        else:
            blocks = [("", rs)]

        default = "thr" if thr_series(blocks[0][1], area) else "time"
        title = html.escape(bench) + (f" <span class='vsuf'>· {html.escape(variant)}</span>" if variant else "")
        n_ok = sum(1 for r in rs if r.get("status") == "ok")
        n_sk = sum(1 for r in rs if r.get("status") in ("skipped", "invalid"))
        meta = (f"<span class='pill'>{html.escape(area)}</span>"
                f"<span class='pill'>{n_ok} measured</span>"
                + (f"<span class='pill'>{n_sk} skipped</span>" if n_sk else ""))

        sel = ""
        if multi:
            opts = "".join(f"<option value='{slug(d)}'>{html.escape(d)}</option>" for d, _ in blocks)
            sel = (f"<label class='dslabel'>dataset"
                   f"<select class='dssel' onchange=\"showDataset('{key}',this.value)\">{opts}</select></label>")

        tabbar = "<div class='tabs'>" + "".join(
            f"<button class='tabb{' active' if m == default else ''}' data-m='{m}' "
            f"onclick=\"showTab('{key}','{m}')\">{name}</button>" for m, name in metrics) + "</div>"

        dsblocks = []
        for i, (d, drows) in enumerate(blocks):
            meas = measured(drows)
            skipped = [r for r in drows if r.get("status") in ("skipped", "invalid")]
            sers = {"thr": thr_series(drows, area), "time": time_series(drows), "mem": mem_series(drows)}
            names = {"thr": "Throughput", "time": "Time", "mem": "Memory"}
            tabcs = "".join(
                f"<div class='tabc tabc-{m}{' show' if m == default else ''}'>"
                f"{render_tab(sers[m], meas, skipped, names[m])}</div>" for m in ("thr", "time", "mem"))
            disp = "block" if i == 0 else "none"
            dsblocks.append(f"<div class='dsblock' data-ds='{slug(d)}' style='display:{disp}'>{tabcs}</div>")

        main.append(
            f"<section class='panel{' show' if first else ''}' id='panel-{key}'>"
            f"<h2>{title}</h2><div class='meta'>{meta}</div>"
            f"{sel}{tabbar}{''.join(dsblocks)}</section>")
        first = False
    main.append("</main>")

    head = ("<!doctype html><html lang='en' data-theme='light'><head>"
            "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>Cajeta profile — benchmark report</title>"
            f"<style>{CSS}</style></head><body>")
    header = ("<header><button class='toggle' onclick='tog()'>◑ theme</button>"
              "<h1>🍮 Cajeta profile</h1>"
              "<div class='sub'>cross-language benchmark &amp; diagnostic suite — taller bar = better, on the machine below</div>"
              "</header>")
    foot = ("<footer>Generated by <code>samples/profile/report/report.py</code> from results.csv — "
            "the interim reporter (a Cajeta-native one replaces it once the language ships dataframe/"
            "plotting). Cajeta numbers measured <code>--release</code>.</footer>")
    script = """<script>
function showBench(k){
  document.querySelectorAll('.panel').forEach(function(p){p.classList.remove('show')});
  document.querySelectorAll('.navbench').forEach(function(n){n.classList.remove('active')});
  var p=document.getElementById('panel-'+k); if(p)p.classList.add('show');
  var n=document.getElementById('nav-'+k); if(n){n.classList.add('active');
    n.scrollIntoView({block:'nearest'});}
}
function applyTab(p,m){
  p.querySelectorAll('.dsblock').forEach(function(blk){
    var vis=blk.style.display!=='none';
    blk.querySelectorAll('.tabc').forEach(function(t){t.classList.remove('show')});
    if(vis){var c=blk.querySelector('.tabc-'+m); if(c)c.classList.add('show');}
  });
}
function showTab(k,m){
  var p=document.getElementById('panel-'+k); if(!p)return;
  p.querySelectorAll('.tabb').forEach(function(t){t.classList.remove('active')});
  var b=p.querySelector('.tabb[data-m="'+m+'"]'); if(b)b.classList.add('active');
  applyTab(p,m);
}
function showDataset(k,ds){
  var p=document.getElementById('panel-'+k); if(!p)return;
  p.querySelectorAll('.dsblock').forEach(function(b){b.style.display='none'});
  var b=p.querySelector('.dsblock[data-ds="'+ds+'"]'); if(b)b.style.display='block';
  var act=p.querySelector('.tabb.active');
  applyTab(p, act?act.getAttribute('data-m'):'thr');
}
function tog(){var r=document.documentElement;r.dataset.theme=r.dataset.theme==='dark'?'light':'dark';}
(function(){var f=document.querySelector('.navbench');if(f)f.classList.add('active');})();
</script>"""
    return head + header + "<div class='app'>" + "".join(sb) + "".join(main) + "</div>" + foot + script + "</body></html>"


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "results/latest"
    rows = read_csv(os.path.join(d, "results.csv"))
    env = read_env(os.path.join(d, "env.csv"))
    with open(os.path.join(d, "report.md"), "w") as f:
        f.write(gen_markdown(rows, env))
    site = os.path.join(d, "site")
    os.makedirs(site, exist_ok=True)
    with open(os.path.join(site, "index.html"), "w") as f:
        f.write(gen_html(rows, env))
    print(f"report: wrote {d}/report.md and {site}/index.html "
          f"({len(rows)} rows, {len(benchmarks(rows))} benchmarks, {len(group_by_area(rows))} areas)")


if __name__ == "__main__":
    main()
