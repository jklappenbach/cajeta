// Pure helpers for the Benchmarks section (benchmark-reference §5): the three
// fenced blocks' grammar, value formatting, the chart / standings / matrix
// models, the default-machine rule, and the static no-JS fallback HTML. Shared
// by the rehype plugin (src/lib/rehype-bench.mjs), the page templates (which
// re-fill the placeholders on every build, see fillPlaceholders) and the React
// island (src/components/react/BenchChart.tsx). No node imports: browser-safe.

export const METRICS = ['time', 'throughput', 'memory'];

/** Machines in selector order, and the default (spec §12.3). */
export const MACHINE_ORDER = ['phoenix', 'proton'];
export const DEFAULT_MACHINE = 'proton';
/** The CUDA box: the default on xpu pages whose rows are cuda-only (§12.3). */
export const CUDA_MACHINE = 'phoenix';
const CUDA_BACKENDS = new Set(['cuda', 'nvptx']);

const LANGUAGE_LABELS = {
  cajeta: 'Cajeta',
  c: 'C',
  cpp: 'C++',
  'c++': 'C++',
  rust: 'Rust',
  go: 'Go',
  java: 'Java',
  python: 'Python',
  cpython: 'CPython',
  numpy: 'NumPy',
  pytorch: 'PyTorch',
  'cuda-c++': 'CUDA C++',
  cuda: 'CUDA',
  'hip-c++': 'HIP C++',
  hip: 'HIP',
  csharp: 'C#',
  'c#': 'C#',
  kotlin: 'Kotlin',
  node: 'Node',
  javascript: 'JavaScript',
  typescript: 'TypeScript',
  zig: 'Zig',
};

/** Display name of a CSV `language` value ('cpp' -> 'C++'). */
export function languageLabel(language) {
  const key = String(language ?? '').trim();
  if (!key) return 'unknown';
  return LANGUAGE_LABELS[key.toLowerCase()] ?? key.charAt(0).toUpperCase() + key.slice(1);
}

/** 'phoenix' -> 'Phoenix'. */
export function machineLabel(name) {
  const s = String(name ?? '');
  return s ? s.charAt(0).toUpperCase() + s.slice(1) : s;
}

/** A row measured by Cajeta itself (language cajeta, or a cajeta-* platform). */
export function isCajetaRow(row) {
  return /^cajeta/i.test(row.language ?? '') || /^cajeta/i.test(row.platform ?? '');
}

// ------------------------------------------------------------------ blocks

const BLOCK_KINDS = { bench: 'chart', 'bench-standings': 'standings', 'bench-matrix': 'matrix' };

/** Fence language -> block kind, or null for any other fence. */
export function blockKindFor(lang) {
  return BLOCK_KINDS[lang] ?? null;
}

const list = (v) =>
  String(v ?? '')
    .split(',')
    .map((s) => s.trim())
    .filter(Boolean);

/**
 * Parse one fenced block's `key: value` lines. Throws (without a page path;
 * the caller adds it) on a missing id, an unknown metric, an unknown key, or a
 * matrix id that matches none of its measures.
 */
export function parseBlock(kind, text) {
  const fields = {};
  for (const raw of String(text ?? '').split(/\r?\n/)) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) continue;
    const m = /^([a-z-]+)\s*:\s*(.*)$/i.exec(line);
    if (!m) throw new Error(`cannot read the ${kind} block line "${line}"`);
    fields[m[1].toLowerCase()] = m[2].trim();
  }
  const allowed = { chart: ['id', 'metric'], standings: [], matrix: ['keys', 'measures', 'ids'] }[kind];
  for (const k of Object.keys(fields)) {
    if (!allowed.includes(k)) throw new Error(`the ${kind} block has an unknown key "${k}"`);
  }
  if (kind === 'chart') {
    if (!fields.id) throw new Error('a bench block needs an id');
    const metric = fields.metric || 'time';
    if (!METRICS.includes(metric)) {
      throw new Error(`bench block ${fields.id}: metric "${metric}" is not one of ${METRICS.join(', ')}`);
    }
    return { kind, id: fields.id, metric };
  }
  if (kind === 'standings') return { kind };
  const keys = list(fields.keys);
  const measures = list(fields.measures);
  const ids = list(fields.ids);
  if (!keys.length || !measures.length || !ids.length) {
    throw new Error('a bench-matrix block needs keys, measures and ids');
  }
  for (const id of ids) {
    if (!measureOf(id, measures)) {
      throw new Error(`bench-matrix id ${id} matches none of the measures ${measures.join(', ')}`);
    }
  }
  return { kind, keys, measures, ids };
}

/** The ids a block renders. */
export function blockIds(block) {
  return block.kind === 'chart' ? [block.id] : block.kind === 'matrix' ? block.ids : [];
}

/** Block -> the data-* attributes its placeholder carries. */
export function blockAttrs(block) {
  if (block.kind === 'chart') return { 'data-kind': 'chart', 'data-id': block.id, 'data-metric': block.metric };
  if (block.kind === 'matrix') {
    return {
      'data-kind': 'matrix',
      'data-keys': block.keys.join(','),
      'data-measures': block.measures.join(','),
      'data-ids': block.ids.join(','),
    };
  }
  return { 'data-kind': 'standings' };
}

/** data-* attributes (as a plain object, or an Element's dataset) -> block. */
export function blockFromAttrs(get) {
  const kind = get('kind');
  if (kind === 'chart') return { kind, id: get('id'), metric: get('metric') || 'time' };
  if (kind === 'matrix') return { kind, keys: list(get('keys')), measures: list(get('measures')), ids: list(get('ids')) };
  return { kind: 'standings' };
}

/** The measure (column group) a matrix id belongs to: llm/decode-1seq -> decode. */
export function measureOf(id, measures) {
  const name = id.split('/').pop();
  return measures.find((m) => name === m || name.startsWith(m + '-')) ?? null;
}

// ------------------------------------------------------------------ formatting

function sig3(x) {
  const a = Math.abs(x);
  if (a >= 100) return Math.round(x).toLocaleString('en-US');
  if (a === 0) return '0';
  return x.toPrecision(3);
}

/** Nanoseconds by magnitude: 950 ns, 1.20 µs, 3.40 ms, 1.25 s. */
export function formatNs(ns) {
  if (ns === null || ns === undefined || !Number.isFinite(ns)) return '—';
  if (ns < 1e3) return `${sig3(ns)} ns`;
  if (ns < 1e6) return `${sig3(ns / 1e3)} µs`;
  if (ns < 1e9) return `${sig3(ns / 1e6)} ms`;
  return `${sig3(ns / 1e9)} s`;
}

/** Bytes by magnitude (binary steps): 512 B, 781 KB, 3.20 MB, 1.10 GB. */
export function formatBytes(b) {
  if (b === null || b === undefined || !Number.isFinite(b)) return '—';
  if (b < 1024) return `${Math.round(b)} B`;
  if (b < 1024 ** 2) return `${sig3(b / 1024)} KB`;
  if (b < 1024 ** 3) return `${sig3(b / 1024 ** 2)} MB`;
  return `${sig3(b / 1024 ** 3)} GB`;
}

export function formatThroughput(v, unit) {
  if (v === null || v === undefined || !Number.isFinite(v)) return '—';
  return `${sig3(v)}${unit ? ' ' + unit : ''}`;
}

export const formatTps = (v) => formatThroughput(v, 'tok/s');
export const formatRatio = (r) => (r === null || r === undefined || !Number.isFinite(r) ? '—' : `${r.toFixed(2)}×`);

// ------------------------------------------------------------------ measures

/**
 * What a chart can plot. `time` is median_ns, `throughput` the row's
 * throughput with its unit, `alloc` alloc_bytes, `memory` peak RSS.
 */
export const MEASURES = {
  time: { label: 'Time', better: 'lower', value: (e) => e.median, format: (v) => formatNs(v) },
  throughput: {
    label: 'Throughput',
    better: 'higher',
    value: (e) => e.throughput,
    format: (v, e) => formatThroughput(v, e?.unit),
  },
  alloc: { label: 'Allocation', better: 'lower', value: (e) => e.alloc, format: (v) => formatBytes(v) },
  memory: {
    label: 'Memory',
    better: 'lower',
    value: (e) => e.rss,
    format: (v) => formatBytes(v === null || v === undefined ? v : v * 1024),
  },
};

/** The segmented control's three measures for a block's metric. */
export function measuresFor(metric) {
  return [metric === 'throughput' ? 'throughput' : 'time', 'alloc', 'memory'];
}

/** The measure a block opens on. */
export const initialMeasure = (metric) => (metric === 'memory' ? 'memory' : metric);

/** "skipped · reason" / "failed · reason" for a row with no measurement. */
export function statusText(e) {
  if (e.status === 'skipped') return `skipped · ${e.reason || 'no reason recorded'}`;
  if (e.status === 'failed') return `failed · ${e.reason || 'no message recorded'}`;
  if (e.status === 'invalid') return `invalid · ${e.reason || 'the correctness check failed'}`;
  return '';
}

const RANK = { ok: 0, invalid: 2, failed: 3, skipped: 4 };

/**
 * One chart's model: per variant, its rows best-first (by the measure's
 * direction), unmeasured rows (skipped / failed / no value) after. `max` is
 * shared across the variants so the small multiples share one x scale.
 */
export function chartModel(variants, measureKey) {
  const measure = MEASURES[measureKey];
  let max = 0;
  const out = Object.keys(variants ?? {})
    .sort((a, b) => (a === '' ? -1 : b === '' ? 1 : 0))
    .map((variant) => {
      const rows = Object.entries(variants[variant]).map(([label, entry]) => {
        const raw = entry.status === 'ok' ? measure.value(entry) : null;
        const value = raw !== null && raw !== undefined && Number.isFinite(raw) && raw > 0 ? raw : null;
        if (value !== null) max = Math.max(max, value);
        const state = entry.status !== 'ok' ? entry.status : value === null ? 'none' : 'ok';
        const text =
          state === 'ok'
            ? measure.format(value, entry)
            : state === 'none'
              ? 'not recorded'
              : statusText(entry);
        return { label, entry, value, state, text };
      });
      rows.sort((a, b) => {
        if (a.value !== null && b.value !== null) {
          return measure.better === 'lower' ? a.value - b.value : b.value - a.value;
        }
        if (a.value !== null) return -1;
        if (b.value !== null) return 1;
        return (RANK[a.state] ?? 1) - (RANK[b.state] ?? 1) || a.label.localeCompare(b.label);
      });
      return { variant, rows };
    });
  return { measure: measureKey, better: measure.better, variants: out, max };
}

// ------------------------------------------------------------------ row labels

/** Label sizes (px) and weights: the language line, and the library line under it. */
export const LABEL_PRIMARY = { size: 12, weight: 400, cajetaWeight: 600 };
export const LABEL_SECONDARY = { size: 10.5, weight: 400 };
/** Gap between the label column's text and the bar baseline, plus the left margin. */
export const LABEL_GAP = 10;
export const LABEL_MARGIN = 4;

/**
 * A library's short name for the second label line: the standard library is
 * `std` however it is spelled (`std::unordered_map`, `std-HashMap`); anything
 * else drops its namespaces and keeps the last segment
 * (`ankerl::unordered_dense` -> `unordered_dense`, `ahash` -> `ahash`).
 */
export function libraryShortName(name) {
  const s = String(name ?? '').trim();
  if (!s) return '';
  if (/^std(::|-|$)/i.test(s)) return 'std';
  const parts = s.split('::').filter(Boolean);
  return parts[parts.length - 1] ?? s;
}

/**
 * The two-line row labels of one chart. Line one is the language (or the
 * platform when a row has no language). Line two appears only on rows whose
 * language is repeated in the chart: the library's short name, or its full
 * name, then its backend, when the short names collide. Nothing is ever
 * shortened with an ellipsis; the label column widens instead
 * (labelColumnWidth).
 */
export function rowLabelLines(entries) {
  const primary = entries.map((e) => (String(e.language ?? '').trim() ? languageLabel(e.language) : e.platform || 'unknown'));
  const count = (arr, v) => arr.filter((x) => x === v).length;
  const lines = entries.map((e, i) => ({ primary: primary[i], secondary: null, cajeta: !!e.cajeta }));
  const groups = new Map();
  primary.forEach((p, i) => groups.set(p, [...(groups.get(p) ?? []), i]));
  for (const members of groups.values()) {
    if (members.length < 2) continue;
    const tries = [
      (e) => libraryShortName(e.library || e.platform),
      (e) => e.library || e.platform || '',
      (e) => [e.library || e.platform, e.backend].filter(Boolean).join(' · '),
    ];
    let names = members.map((i) => tries[0](entries[i]));
    for (const t of tries.slice(1)) {
      if (names.every((n, k) => n && count(names, n) === 1)) break;
      names = members.map((i) => t(entries[i]));
    }
    members.forEach((i, k) => {
      lines[i].secondary = names[k] || null;
    });
  }
  return lines;
}

/**
 * The label column's width: the widest single line as `measure(text, size,
 * weight)` reports it, plus the gap to the bars and the left margin.
 */
export function labelColumnWidth(lines, measure) {
  let widest = 0;
  for (const l of lines) {
    widest = Math.max(
      widest,
      measure(l.primary, LABEL_PRIMARY.size, l.cajeta ? LABEL_PRIMARY.cajetaWeight : LABEL_PRIMARY.weight)
    );
    if (l.secondary) widest = Math.max(widest, measure(l.secondary, LABEL_SECONDARY.size, LABEL_SECONDARY.weight));
  }
  return Math.ceil(widest) + LABEL_GAP + LABEL_MARGIN;
}

// ------------------------------------------------------------------ machines

/**
 * The selector's machines: those with a run under bench/results/, in
 * MACHINE_ORDER; with no results at all, Phoenix and Proton, disabled.
 */
export function machineOptions(data) {
  const machines = data?.machines ?? [];
  if (!machines.length) {
    return MACHINE_ORDER.map((name) => ({ name, label: machineLabel(name), run: null, date: '', disabled: true }));
  }
  const rank = (n) => {
    const i = MACHINE_ORDER.indexOf(n);
    return i === -1 ? MACHINE_ORDER.length : i;
  };
  return [...machines]
    .sort((a, b) => rank(a.name) - rank(b.name) || a.name.localeCompare(b.name))
    .map((m) => ({ name: m.name, label: m.label ?? machineLabel(m.name), run: m.run, date: m.date ?? '', disabled: false }));
}

function rowsOn(data, machine, ids) {
  const res = data?.results?.[machine] ?? {};
  const rows = [];
  for (const id of ids) {
    for (const variant of Object.values(res[id] ?? {})) rows.push(...Object.values(variant));
  }
  return rows;
}

/**
 * The machine a page opens on (spec §12.3): Proton, except on an xpu page
 * whose rows are cuda-only, where Phoenix is the default when it has rows for
 * the page and Proton has none. When Proton has no run at all, the first
 * machine that has one.
 */
export function defaultMachine(pagePath, ids, data) {
  const path = String(pagePath ?? '').replace(/^bench\//, 'benchmarks/');
  if (/^benchmarks\/xpu\//.test(path)) {
    const cuda = rowsOn(data, CUDA_MACHINE, ids);
    const other = rowsOn(data, DEFAULT_MACHINE, ids);
    if (cuda.length && !other.length && cuda.every((r) => CUDA_BACKENDS.has(String(r.backend).toLowerCase()))) {
      return CUDA_MACHINE;
    }
  }
  const machines = data?.machines ?? [];
  if (!machines.length || machines.some((m) => m.name === DEFAULT_MACHINE)) return DEFAULT_MACHINE;
  return machineOptions(data)[0].name;
}

const runOf = (data, machine) => (data?.machines ?? []).find((m) => m.name === machine) ?? null;

// ------------------------------------------------------------------ standings

/**
 * The landing standings for one machine (spec §5.3): per package, the
 * geometric mean of Cajeta's time ratio to the best competitor over its
 * benchmarks (each variant counts) that have both, plus wins, losses and
 * skips (a benchmark with rows on the machine but no comparable pair). The
 * time ratio is Cajeta median / best competitor median; for a throughput
 * benchmark whose rows carry throughput it is best competitor throughput /
 * Cajeta throughput, the same "lower is better" ratio. Memory-metric
 * benchmarks are left out: their time is not what they measure. `packages`
 * orders the rows and, when given, limits them to the catalog's packages.
 */
export function standingsModel(machineResults, { idMeta = {}, packages = [] } = {}) {
  const res = machineResults ?? {};
  const pkgOf = (id) => id.split('/')[0];
  const inCatalog = (id) => !packages.length || packages.includes(pkgOf(id));
  const ids = new Set(
    [...Object.keys(idMeta), ...Object.keys(res)].filter((id) => idMeta[id]?.metric !== 'memory' && inCatalog(id))
  );
  const byPkg = new Map(packages.map((p) => [p, { pkg: p, logs: [], wins: 0, losses: 0, skips: 0 }]));
  for (const id of ids) {
    const p = pkgOf(id);
    if (!byPkg.has(p)) byPkg.set(p, { pkg: p, logs: [], wins: 0, losses: 0, skips: 0 });
    const acc = byPkg.get(p);
    for (const variant of Object.values(res[id] ?? {})) {
      const ok = Object.values(variant).filter((e) => e.status === 'ok');
      const byRate = idMeta[id]?.metric === 'throughput' && ok.some((e) => e.cajeta && e.throughput > 0);
      const pick = (side) =>
        ok
          .filter((e) => !!e.cajeta === side)
          .map((e) => (byRate ? e.throughput : e.median))
          .filter((v) => v > 0);
      const ours = pick(true);
      const theirs = pick(false);
      if (!ours.length || !theirs.length) {
        acc.skips++;
        continue;
      }
      const ratio = byRate ? Math.max(...theirs) / Math.max(...ours) : Math.min(...ours) / Math.min(...theirs);
      acc.logs.push(Math.log(ratio));
      if (ratio < 1) acc.wins++;
      else if (ratio > 1) acc.losses++;
    }
  }
  const order = (p) => {
    const i = packages.indexOf(p);
    return i === -1 ? packages.length : i;
  };
  return [...byPkg.values()]
    .sort((a, b) => order(a.pkg) - order(b.pkg) || a.pkg.localeCompare(b.pkg))
    .map(({ pkg, logs, wins, losses, skips }) => ({
      pkg,
      ratio: logs.length ? Math.exp(logs.reduce((s, x) => s + x, 0) / logs.length) : null,
      n: logs.length,
      wins,
      losses,
      skips,
    }));
}

// ------------------------------------------------------------------ matrix

const platformName = (p, fallback) => String(p ?? '').trim().split(/\s+/)[0] || fallback;

/**
 * The llm matrix for one machine (spec §7.1): one block per model, rows the
 * distinct key tuples, a cell per id holding the best cajeta-llm and the best
 * competitor throughput and their ratio.
 */
export function matrixModel(machineResults, block) {
  const res = machineResults ?? {};
  const columns = block.measures.map((measure) => ({
    measure,
    ids: block.ids
      .filter((id) => measureOf(id, block.measures) === measure)
      .map((id) => {
        const name = id.split('/').pop();
        return { id, label: name === measure ? measure : name.slice(measure.length + 1) };
      }),
  }));
  const models = new Map();
  for (const id of block.ids) {
    for (const variant of Object.values(res[id] ?? {})) {
      for (const e of Object.values(variant)) {
        if (e.status !== 'ok' || !(e.throughput > 0)) continue;
        const model = e.model || 'model not recorded';
        if (!models.has(model)) models.set(model, { model, rows: new Map(), names: { cajeta: '', other: '' } });
        const m = models.get(model);
        const keyVals = block.keys.map((k) => String(e[k] ?? '').trim() || '—');
        const key = keyVals.join('\u0000');
        if (!m.rows.has(key)) m.rows.set(key, { key: keyVals, cells: {} });
        const cell = (m.rows.get(key).cells[id] ??= { cajeta: null, other: null, ratio: null });
        const side = e.cajeta ? 'cajeta' : 'other';
        if (cell[side] === null || e.throughput > cell[side]) cell[side] = e.throughput;
        if (!m.names[side]) m.names[side] = platformName(e.platform, e.cajeta ? 'cajeta-llm' : languageLabel(e.language));
      }
    }
  }
  const out = [...models.values()]
    .sort((a, b) => a.model.localeCompare(b.model))
    .map((m) => {
      const rows = [...m.rows.values()].sort((a, b) => a.key.join(' ').localeCompare(b.key.join(' ')));
      for (const r of rows) {
        for (const c of Object.values(r.cells)) c.ratio = c.cajeta && c.other ? c.cajeta / c.other : null;
      }
      return {
        model: m.model,
        rows,
        names: { cajeta: m.names.cajeta || 'cajeta-llm', other: m.names.other || 'llama.cpp' },
      };
    });
  return { keys: block.keys, columns, models: out };
}

// ------------------------------------------------------------------ fallback HTML

export function escHtml(s) {
  return String(s ?? '')
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;');
}

const td = (s) => `<td>${escHtml(s)}</td>`;
const th = (s) => `<th>${escHtml(s)}</th>`;

/** "library and version" cell text. */
export function versionText(e) {
  return [e.versions?.language, e.versions?.library].filter(Boolean).join(' · ');
}

export function libraryText(e) {
  return e.library || (e.platform && !/^cajeta$/i.test(e.platform) ? e.platform : '');
}

/** status / notes cell text. */
export function notesText(e) {
  return e.status === 'ok' ? e.reason || 'ok' : statusText(e);
}

export const CHART_COLUMNS = ['language', 'library', 'version', 'median', 'min', 'p95', 'alloc bytes', 'peak RSS', 'status / notes'];

/** The data table of one chart's variant rows (the CorpusCharts fallback pattern). */
export function chartTableHtml(model) {
  return model.variants
    .map(({ variant, rows }) => {
      const body = rows
        .map(
          ({ label, entry: e }) =>
            `<tr>${td(label)}${td(libraryText(e))}${td(versionText(e))}${td(formatNs(e.median))}${td(formatNs(e.min))}${td(formatNs(e.p95))}${td(formatBytes(e.alloc))}${td(MEASURES.memory.format(e.rss))}${td(notesText(e))}</tr>`
        )
        .join('');
      const caption = variant ? `<caption>${escHtml(variant)}</caption>` : '';
      return `<table>${caption}<thead><tr>${CHART_COLUMNS.map(th).join('')}</tr></thead><tbody>${body}</tbody></table>`;
    })
    .join('');
}

export const unmeasuredText = (machine) => `Not yet measured on ${machineLabel(machine)}.`;

export function captionText(subject, better, machine, date) {
  return [subject, `${better} is better`, machineLabel(machine), date].filter(Boolean).join(' · ');
}

function unmeasuredHtml(caption, machine) {
  return `<figure class="chart bench-figure bench-unmeasured-figure"><figcaption>${escHtml(caption)}</figcaption><p class="bench-unmeasured">${escHtml(unmeasuredText(machine))}</p></figure>`;
}

export function standingsTableHtml(rows) {
  const body = rows
    .map((r) => `<tr>${td(r.pkg)}${td(formatRatio(r.ratio))}${td(r.wins)}${td(r.losses)}${td(r.skips)}</tr>`)
    .join('');
  return `<table><thead><tr>${['package', 'geomean ratio', 'wins', 'losses', 'skips'].map(th).join('')}</tr></thead><tbody>${body}</tbody></table>`;
}

export function matrixTableHtml(model, m) {
  const head = [
    ...model.keys,
    ...model.columns.flatMap((c) =>
      c.ids.flatMap((i) => [`${c.measure} ${i.label} · ${m.names.cajeta}`, `${m.names.other}`, 'ratio'])
    ),
  ];
  const body = m.rows
    .map((r) => {
      const cells = model.columns.flatMap((c) =>
        c.ids.flatMap((i) => {
          const cell = r.cells[i.id] ?? {};
          return [td(formatTps(cell.cajeta)), td(formatTps(cell.other)), td(formatRatio(cell.ratio))];
        })
      );
      return `<tr>${r.key.map(td).join('')}${cells.join('')}</tr>`;
    })
    .join('');
  return `<table><thead><tr>${head.map(th).join('')}</tr></thead><tbody>${body}</tbody></table>`;
}

export const STANDINGS_SUBJECT = 'Standings · Cajeta time ÷ best competitor, geometric mean';
export const matrixSubject = (block) => block.measures.join(' · ') + ' · tokens/s';

/**
 * The static HTML inside one placeholder for `machine`: the unmeasured state,
 * or the figure with its Data table (the island replaces the figure with the
 * chart and keeps the table under it).
 */
export function fallbackHtml(block, { machine, data }) {
  const res = data?.results?.[machine] ?? {};
  const run = runOf(data, machine);
  const date = run?.date ?? '';
  if (block.kind === 'chart') {
    const variants = res[block.id];
    if (!variants || !Object.keys(variants).length) return unmeasuredHtml(block.id, machine);
    const model = chartModel(variants, initialMeasure(block.metric));
    const caption = captionText(block.id, MEASURES[initialMeasure(block.metric)].better, machine, date);
    return `<figure class="chart bench-figure"><figcaption>${escHtml(caption)}</figcaption><details class="bench-data"><summary>Data</summary>${chartTableHtml(model)}</details></figure>`;
  }
  if (block.kind === 'standings') {
    if (!Object.keys(res).length) return unmeasuredHtml('Standings', machine);
    const rows = standingsModel(res, { idMeta: data?.idMeta, packages: data?.packages });
    const caption = captionText(STANDINGS_SUBJECT, 'lower', machine, date);
    return `<figure class="chart bench-figure"><figcaption>${escHtml(caption)}</figcaption><details class="bench-data"><summary>Data</summary>${standingsTableHtml(rows)}</details></figure>`;
  }
  const model = matrixModel(res, block);
  if (!model.models.length) return unmeasuredHtml(matrixSubject(block), machine);
  return model.models
    .map((m) => {
      const caption = captionText(`${m.model} · tokens/s`, 'higher', machine, date);
      return `<figure class="chart bench-figure"><figcaption>${escHtml(caption)}</figcaption><details class="bench-data"><summary>Data</summary>${matrixTableHtml(model, m)}</details></figure>`;
    })
    .join('');
}

// ------------------------------------------------------------------ pages

/** bench-relative or pseudo path -> the repository path errors name. */
export const srcPathOf = (pagePath) => String(pagePath).replace(/^benchmarks\//, 'bench/');

/** Throw when a block names an id outside `knownIds` (when a set is given). */
export function checkKnownIds(block, pagePath, knownIds) {
  if (!knownIds) return;
  const known = knownIds instanceof Set ? knownIds : new Set(knownIds);
  for (const id of blockIds(block)) {
    if (!known.has(id)) {
      throw new Error(`${srcPathOf(pagePath)}: unknown benchmark id "${id}" (not registered by the harness and not in any result)`);
    }
  }
}

const PLACEHOLDER = /<bench-chart((?:\s+[a-z-]+="[^"]*")*)\s*>[\s\S]*?<\/bench-chart>/g;

function attrsOf(s) {
  const out = {};
  for (const m of s.matchAll(/([a-z-]+)="([^"]*)"/g)) out[m[1]] = m[2];
  return out;
}

const attrString = (attrs) =>
  Object.entries(attrs)
    .map(([k, v]) => ` ${k}="${escHtml(v)}"`)
    .join('');

/** Every block in rendered page HTML. */
export function placeholderBlocks(html) {
  return [...String(html).matchAll(PLACEHOLDER)].map((m) => {
    const a = attrsOf(m[1]);
    return blockFromAttrs((k) => a[`data-${k}`]);
  });
}

/**
 * Re-fill every placeholder in rendered page HTML against `data`. The content
 * layer caches rendered markdown by file digest, so the fallback a cached page
 * carries can be from an older results set; the page templates call this on
 * every build so the static HTML always matches bench/results/.
 */
export function fillPlaceholders(html, { pagePath, data, machine }) {
  const blocks = placeholderBlocks(html);
  const ids = blocks.flatMap(blockIds);
  const m = machine ?? defaultMachine(pagePath, ids, data);
  return String(html).replace(PLACEHOLDER, (_all, attrs) => {
    const a = attrsOf(attrs);
    const block = blockFromAttrs((k) => a[`data-${k}`]);
    checkKnownIds(block, pagePath, data?.knownIds);
    return `<bench-chart${attrString(blockAttrs(block))}>${fallbackHtml(block, { machine: m, data })}</bench-chart>`;
  });
}

/**
 * Props for the page's island: the selector's machines, the page's default,
 * and the page's rows for every machine (so switching is client-side).
 */
export function islandProps(pagePath, blocks, data) {
  const ids = [...new Set(blocks.flatMap(blockIds))];
  const machines = machineOptions(data);
  const results = {};
  const standings = {};
  const hasStandings = blocks.some((b) => b.kind === 'standings');
  for (const { name } of machines) {
    const all = data?.results?.[name] ?? {};
    results[name] = Object.fromEntries(ids.filter((id) => all[id]).map((id) => [id, all[id]]));
    if (hasStandings && Object.keys(all).length) {
      standings[name] = standingsModel(all, { idMeta: data?.idMeta, packages: data?.packages });
    }
  }
  return {
    pagePath,
    machines,
    defaultMachine: defaultMachine(pagePath, ids, data),
    results,
    standings,
  };
}
