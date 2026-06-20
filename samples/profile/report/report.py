#!/usr/bin/env python3
"""profile report generator (Unit 16).

Reads the raw benchmark CSVs (results.csv + env.csv) and emits, from the data
ONLY (it never runs a benchmark):

  * report.md            — grouped Markdown comparison tables
  * site/index.html      — a self-contained, Cajeta-themed static minisite with a
                           per-area sortable table + pure-CSS throughput bars,
                           the reference-machine env block, and a light/dark toggle.

Interim reporter: once Cajeta has a dataframe/plotting capability a Cajeta-native
reporter replaces this, consuming the same CSV. Pure stdlib (csv) — no deps.

Usage: report.py <results-dir>        # reads <dir>/results.csv + <dir>/env.csv,
                                       # writes <dir>/report.md + <dir>/site/
"""
import csv
import html
import os
import sys

# Cajeta brand (from cajeta-docs-site): primary blue, secondary, purple accent,
# deep-navy dark page.
PRIMARY = "#0161EF"
SECONDARY = "#0154CF"
ACCENT = "#6D28D9"
ACCENT2 = "#8D46E7"
DARK = "#030621"


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


def ms(ns):
    n = num(ns)
    return "" if n is None or n < 0 else f"{n / 1e6:.3f}"


def rate(size, ns):
    s, n = num(size), num(ns)
    if s is None or n is None or n <= 0 or s <= 0:
        return ""
    r = s * 1e9 / n  # size units per second
    for unit, div in (("G", 1e9), ("M", 1e6), ("K", 1e3)):
        if r >= div:
            return f"{r / div:.2f}{unit}/s"
    return f"{r:.0f}/s"


AREAS_ORDER = ["codec", "collection", "sort", "string", "hash", "stream",
               "math", "clbg", "time", "concurrent", "xpu"]


def group_by_area(rows):
    g = {}
    for r in rows:
        area = r.get("area", "")
        if area in ("scaffold", "selftest", "none", ""):
            continue
        g.setdefault(area, []).append(r)
    order = AREAS_ORDER + [a for a in g if a not in AREAS_ORDER]
    return [(a, g[a]) for a in order if a in g]


# ---------------------------------------------------------------- Markdown ----
def gen_markdown(rows, env):
    out = ["# Cajeta profile — benchmark report", ""]
    if env:
        out.append("Reference machine: " + " · ".join(
            f"**{k}** {v}" for k, v in env.items() if k != "run_id") + "\n")
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
CSS = """
:root {{
  --primary: {primary}; --secondary: {secondary}; --accent: {accent};
  --bg: #ffffff; --fg: #0b1020; --muted: #5b6478; --card: #f6f8fc;
  --border: #e3e8f2; --row: #ffffff; --rowalt: #f3f6fd; --bar: {primary};
}}
:root[data-theme="dark"] {{
  --bg: {dark}; --fg: #e6ecff; --muted: #9aa6c4; --card: #0a1030;
  --border: #1b2350; --row: #070c24; --rowalt: #0a1030; --bar: {accent2};
}}
* {{ box-sizing: border-box; }}
body {{ margin:0; font: 15px/1.5 -apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif;
  background: var(--bg); color: var(--fg); }}
header {{ padding: 28px 24px; background: linear-gradient(135deg, {primary}, {accent});
  color: #fff; }}
header h1 {{ margin: 0; font-size: 26px; }}
header .sub {{ opacity: .9; margin-top: 4px; }}
.wrap {{ max-width: 1100px; margin: 0 auto; padding: 24px; }}
.env {{ display:flex; flex-wrap:wrap; gap:8px 18px; background: var(--card);
  border:1px solid var(--border); border-radius:10px; padding:14px 18px; margin: 0 0 24px; }}
.env b {{ color: var(--primary); }}
nav {{ margin: 0 0 18px; }}
nav a {{ display:inline-block; margin:0 10px 8px 0; padding:5px 12px; border-radius:999px;
  background: var(--card); border:1px solid var(--border); color: var(--fg);
  text-decoration:none; font-size:13px; }}
nav a:hover {{ border-color: var(--primary); color: var(--primary); }}
h2 {{ margin: 30px 0 10px; color: var(--primary); border-bottom:2px solid var(--border); padding-bottom:6px; }}
table {{ width:100%; border-collapse: collapse; font-size:13.5px; }}
th, td {{ text-align:left; padding:7px 10px; border-bottom:1px solid var(--border); }}
th {{ cursor:pointer; user-select:none; color: var(--muted); font-weight:600; white-space:nowrap; }}
th:hover {{ color: var(--primary); }}
tr:nth-child(even) td {{ background: var(--rowalt); }}
td.bar-cell {{ width: 200px; }}
.bar {{ height: 14px; border-radius: 3px; background: var(--bar); min-width:2px; }}
.lang-cajeta td:first-child {{ border-left: 3px solid var(--accent); }}
.status-ok {{ color: #1a9e54; font-weight:600; }}
.status-skipped {{ color: #c98a00; }}
.status-invalid {{ color: #d4493f; font-weight:600; }}
.toggle {{ position: fixed; top: 16px; right: 18px; background: rgba(255,255,255,.2);
  border:1px solid rgba(255,255,255,.5); color:#fff; border-radius:999px; padding:6px 12px;
  cursor:pointer; font-size:13px; }}
footer {{ color: var(--muted); font-size:12px; padding: 30px 24px; text-align:center; }}
"""


def bar(ns, maxns):
    n = num(ns)
    if n is None or n <= 0 or not maxns:
        return ""
    import math
    w = int(2 + 196 * (math.log10(max(n, 1)) / math.log10(max(maxns, 10))))
    return f'<div class="bar" style="width:{w}px"></div>'


def gen_html(rows, env):
    areas = group_by_area(rows)
    css = CSS.format(primary=PRIMARY, secondary=SECONDARY, accent=ACCENT,
                     accent2=ACCENT2, dark=DARK)
    parts = ["<!doctype html><html lang='en' data-theme='light'><head>",
             "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>",
             "<title>Cajeta profile — benchmark report</title>",
             f"<style>{css}</style></head><body>",
             "<button class='toggle' onclick='tog()'>◐ theme</button>",
             "<header><div class='wrap' style='padding:0'>",
             "<h1>🚀 Cajeta profile</h1>",
             "<div class='sub'>cross-language benchmark &amp; diagnostic suite — Cajeta results</div>",
             "</div></header><div class='wrap'>"]
    if env:
        parts.append("<div class='env'>" + "".join(
            f"<span><b>{html.escape(k)}</b> {html.escape(v)}</span>"
            for k, v in env.items() if k != "run_id") + "</div>")
    parts.append("<nav>" + "".join(
        f"<a href='#{a}'>{html.escape(a)} ({len(rs)})</a>" for a, rs in areas) + "</nav>")

    cols = [("benchmark", "benchmark"), ("variant", "variant"), ("language", "lang"),
            ("library", "library"),
            ("input_size", "input"), ("min_ns", "min (ms)"), ("median_ns", "median (ms)"),
            ("_rate", "rate"), ("_bar", ""), ("working_set_kb", "ws KB"), ("status", "status")]
    for area, rs in areas:
        maxns = max((num(r.get("min_ns")) or 0) for r in rs)
        parts.append(f"<h2 id='{area}'>{html.escape(area)}</h2>")
        parts.append("<table><thead><tr>" + "".join(
            f"<th onclick='sortT(this,{i})'>{html.escape(lbl)}</th>"
            for i, (_, lbl) in enumerate(cols)) + "</tr></thead><tbody>")
        for r in rs:
            lang = r.get("language", "")
            st = r.get("status", "")
            cells = [
                html.escape(r.get("benchmark", "")), html.escape(r.get("variant", "")),
                html.escape(lang), html.escape(r.get("library", "") or "stdlib"),
                html.escape(r.get("input_size", "")),
                ms(r.get("min_ns")), ms(r.get("median_ns")),
                rate(r.get("input_size"), r.get("min_ns")),
                bar(r.get("min_ns"), maxns), html.escape(r.get("working_set_kb", "")),
                f"<span class='status-{html.escape(st)}'>{html.escape(st)}</span>"]
            # data-sort on min for the bar col so it sorts numerically
            tds = []
            for i, c in enumerate(cells):
                sv = ""
                if cols[i][0] in ("min_ns", "median_ns", "input_size", "working_set_kb"):
                    sv = f" data-sort='{num(r.get(cols[i][0])) if num(r.get(cols[i][0])) is not None else -1}'"
                elif cols[i][0] == "_bar":
                    sv = f" data-sort='{num(r.get('min_ns')) or 0}' class='bar-cell'"
                tds.append(f"<td{sv}>{c}</td>")
            parts.append(f"<tr class='lang-{html.escape(lang)}'>" + "".join(tds) + "</tr>")
        parts.append("</tbody></table>")

    parts.append("</div><footer>Generated by samples/profile/report/report.py from results.csv — "
                 "the interim reporter (a Cajeta-native one replaces it once the language ships "
                 "dataframe/plotting). All numbers measured <code>--release</code> on the machine above.</footer>")
    parts.append("""<script>
function tog(){var r=document.documentElement;r.dataset.theme=r.dataset.theme==='dark'?'light':'dark';}
function sortT(th,i){var tb=th.closest('table').tBodies[0];var rows=[].slice.call(tb.rows);
 var asc=!(th._asc);th._asc=asc;
 rows.sort(function(a,b){var x=cell(a,i),y=cell(b,i);return asc?(x>y?1:x<y?-1:0):(x<y?1:x>y?-1:0);});
 rows.forEach(function(r){tb.appendChild(r);});}
function cell(r,i){var td=r.cells[i];var d=td.getAttribute('data-sort');
 if(d!==null)return parseFloat(d);var t=td.textContent.trim();var n=parseFloat(t);
 return isNaN(n)?t.toLowerCase():n;}
</script></body></html>""")
    return "".join(parts)


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
          f"({len(rows)} rows, {len(group_by_area(rows))} areas)")


if __name__ == "__main__":
    main()
