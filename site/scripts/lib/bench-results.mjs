// Reads the committed benchmark runs (benchmark-reference §4.1) into the data
// the Benchmarks pages render:
//
//   bench/results/<machine>/<run-id>/{results.csv,env.csv}
//   bench/results/<machine>/LATEST          names the run rendered by default
//
// results.csv is the dev.cajeta.bench schema: v2 (37 columns, schema_version 2)
// or v1 (the samples/profile shapes: 32 columns, or the older 31 without
// `backend` and 30 without `variant`). Columns are read by name, so every shape
// loads. Rows tagged `smoke` in notes are never published and are dropped.
//
// The manifest is keyed machine -> benchmark id -> variant -> row label ->
// entry, where the row label is the language ('C++'), told apart by library,
// then backend, then os / driver / model when one benchmark has several rows
// in one language.
//
// BENCHMARK IDS. The id is the catalog path the pages name in their ```bench
// blocks (`collection/hashmap-int`). A row's id is derived from area and
// benchmark (benchIdFor):
//   * area is a catalog path (contains '/'):
//       - the unit-2 harness sets area() to the whole id
//         (area collection/hashmap-int, benchmark hashmap-int) -> the area;
//       - otherwise the first of these the known ids contain:
//         <area>/<benchmark>, <package>/<benchmark>, <area>-<benchmark>
//         (xpu/babelstream + triad -> xpu/babelstream-triad);
//       - else <area>/<benchmark>.
//   * area is an old samples/profile suite name: it maps to its package,
//         hash -> hash/          collection -> collection/   codec -> codec/
//         string -> lang/        stream -> lang/             math -> lang/
//         clbg -> lang/          sort -> collection/         time -> time/
//         concurrent -> concurrent/                          gpu -> xpu/
//     and the id is <package>/<benchmark>, unless only <area>/<benchmark> is a
//     known id (math/matmul stayed in math/).
import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { join, relative } from 'node:path';
import { BENCH_NON_CATALOG_DIRS } from './extract.mjs';
import { languageLabel, isCajetaRow, machineLabel, parseBlock, blockKindFor, blockIds } from './bench-render.mjs';

export const OLD_AREA_PACKAGE = {
  hash: 'hash',
  collection: 'collection',
  codec: 'codec',
  string: 'lang',
  stream: 'lang',
  math: 'lang',
  clbg: 'lang',
  sort: 'collection',
  time: 'time',
  concurrent: 'concurrent',
  gpu: 'xpu',
};

const V1_COLUMNS = (
  'schema_version,run_id,timestamp_unix,benchmark,area,dataset_name,dataset_bytes,' +
  'dataset_sha256,input_size,language,language_version,library,library_version,flags,' +
  'warmup,trials,min_ns,median_ns,mean_ns,p95_ns,throughput,throughput_unit,peak_rss_kb,' +
  'cgroup_peak_kb,alloc_bytes,alloc_count,working_set_kb,status,check_ok,notes,variant,backend'
).split(',');
const V2_COLUMNS = [...V1_COLUMNS, 'machine', 'os', 'driver', 'model', 'platform'];
const KNOWN_HEADERS = [V2_COLUMNS, V1_COLUMNS, V1_COLUMNS.slice(0, -1), V1_COLUMNS.slice(0, -2)].map((h) => h.join(','));

/** RFC 4180 CSV: quoted fields may hold commas, doubled quotes and newlines. */
export function parseCsv(text) {
  const rows = [];
  let row = [];
  let field = '';
  let quoted = false;
  const s = String(text);
  for (let i = 0; i < s.length; i++) {
    const c = s[i];
    if (quoted) {
      if (c === '"') {
        if (s[i + 1] === '"') {
          field += '"';
          i++;
        } else quoted = false;
      } else field += c;
    } else if (c === '"') quoted = true;
    else if (c === ',') {
      row.push(field);
      field = '';
    } else if (c === '\n' || c === '\r') {
      if (c === '\r' && s[i + 1] === '\n') i++;
      row.push(field);
      rows.push(row);
      row = [];
      field = '';
    } else field += c;
  }
  if (field !== '' || row.length) {
    row.push(field);
    rows.push(row);
  }
  return rows.filter((r) => !(r.length === 1 && r[0] === ''));
}

/** The catalog id of a row (see the header comment for the mapping). */
export function benchIdFor(area, benchmark, knownIds) {
  const a = String(area ?? '').trim();
  const b = String(benchmark ?? '').trim();
  const known = (id) => knownIds?.has?.(id) ?? false;
  if (a.includes('/')) {
    if (a.split('/').pop() === b || known(a)) return a;
    const pkg = a.split('/')[0];
    for (const id of [`${a}/${b}`, `${pkg}/${b}`, `${a}-${b}`]) if (known(id)) return id;
    return `${a}/${b}`;
  }
  const mapped = OLD_AREA_PACKAGE[a];
  const candidates = [...new Set([mapped, a].filter(Boolean))].map((p) => `${p}/${b}`);
  return candidates.find(known) ?? candidates[0];
}

const num = (s) => {
  const t = String(s ?? '').trim();
  if (t === '') return null;
  const n = Number(t);
  return Number.isFinite(n) && n >= 0 ? n : null; // -1 is the schema's "not measured"
};

const isSmoke = (notes) => String(notes ?? '').split(';').map((p) => p.trim()).includes('smoke');

/** One CSV record (an object keyed by column) -> a manifest entry. */
export function normalizeRow(r, machine) {
  return {
    language: r.language ?? '',
    cajeta: isCajetaRow(r),
    library: r.library ?? '',
    platform: r.platform ?? '',
    versions: { language: r.language_version ?? '', library: r.library_version ?? '' },
    flags: r.flags ?? '',
    backend: r.backend ?? '',
    machine: (r.machine ?? '').trim() || machine,
    os: r.os ?? '',
    driver: r.driver ?? '',
    model: r.model ?? '',
    variant: r.variant ?? '',
    inputSize: num(r.input_size),
    min: num(r.min_ns),
    median: num(r.median_ns),
    mean: num(r.mean_ns),
    p95: num(r.p95_ns),
    throughput: num(r.throughput),
    unit: r.throughput_unit ?? '',
    alloc: num(r.alloc_bytes),
    allocCount: num(r.alloc_count),
    rss: num(r.peak_rss_kb) ?? num(r.cgroup_peak_kb),
    status: (r.status ?? '').trim() || 'ok',
    check: r.check_ok === 'true',
    reason: (r.notes ?? '').trim(),
  };
}

function readCsvRecords(file, rel) {
  const rows = parseCsv(readFileSync(file, 'utf8'));
  if (!rows.length) return [];
  const header = rows[0].map((h) => h.trim());
  if (!KNOWN_HEADERS.includes(header.join(','))) {
    throw new Error(`${rel}: unrecognized results.csv header (${header.length} columns) — not a schema 1 or 2 shape`);
  }
  return rows.slice(1).map((cells) => Object.fromEntries(header.map((h, i) => [h, cells[i] ?? ''])));
}

function readEnv(file) {
  if (!existsSync(file)) return {};
  const env = {};
  for (const [k, ...v] of parseCsv(readFileSync(file, 'utf8'))) {
    if (!k || (k === 'key' && v[0] === 'value')) continue;
    env[k.trim()] = v.join(',').trim();
  }
  return env;
}

/** YYYY-MM-DD from a run id (20261101T0300Z-…, 20260625-170906), else from timestamps. */
function runDate(runId, records) {
  const m = /^(\d{4})(\d{2})(\d{2})/.exec(runId);
  if (m) return `${m[1]}-${m[2]}-${m[3]}`;
  const ts = Math.max(0, ...records.map((r) => Number(r.timestamp_unix) || 0));
  return ts > 0 ? new Date(ts * 1000).toISOString().slice(0, 10) : '';
}

/**
 * Distinct, readable row labels for one (id, variant)'s entries: the language,
 * and only for rows that share it, the library (or platform), then the
 * backend, then os / driver / model.
 */
function labelEntries(entries) {
  const labels = entries.map((e) => [languageLabel(e.language)]);
  const extras = [
    (e) => e.library || e.platform,
    (e) => e.backend,
    (e) => [e.os, e.driver, e.model].filter(Boolean).join(' · '),
  ];
  for (const extra of extras) {
    const joined = labels.map((p) => p.join(' · '));
    const groups = new Map();
    joined.forEach((j, i) => groups.set(j, [...(groups.get(j) ?? []), i]));
    let changed = false;
    for (const members of groups.values()) {
      if (members.length < 2) continue;
      // append this field only where it tells the colliding rows apart
      const values = members.map((i) => extra(entries[i]));
      if (new Set(values).size < 2) continue;
      members.forEach((i, k) => values[k] && labels[i].push(values[k]));
      changed = true;
    }
    if (!changed && [...groups.values()].every((m) => m.length < 2)) break;
  }
  const seen = new Map();
  return labels.map((p) => {
    const l = p.join(' · ');
    const n = (seen.get(l) ?? 0) + 1;
    seen.set(l, n);
    return n === 1 ? l : `${l} (${n})`;
  });
}

/**
 * Load bench/results/. Options: `knownIds` (a Set, for id resolution),
 * `run` ({machine: run-id} to render a run other than LATEST), `keepSmoke`.
 * Returns { machines: [{name, label, run, runs, date, env}], results }.
 */
export function loadResults(resultsRoot, { knownIds, run = {}, keepSmoke = false } = {}) {
  const machines = [];
  const results = {};
  if (!existsSync(resultsRoot)) return { machines, results };
  const dirs = (p) =>
    readdirSync(p, { withFileTypes: true })
      .filter((d) => d.isDirectory() && !d.name.startsWith('.'))
      .map((d) => d.name)
      .sort();
  for (const machine of dirs(resultsRoot)) {
    const mdir = join(resultsRoot, machine);
    const runs = dirs(mdir).filter((r) => existsSync(join(mdir, r, 'results.csv')));
    if (!runs.length) continue;
    const latestFile = join(mdir, 'LATEST');
    const latest = existsSync(latestFile) ? readFileSync(latestFile, 'utf8').trim() : '';
    const chosen = run[machine] ?? (runs.includes(latest) ? latest : runs[runs.length - 1]);
    if (!runs.includes(chosen)) throw new Error(`bench results: ${machine} has no run ${chosen}`);
    const file = join(mdir, chosen, 'results.csv');
    const rel = relative(join(resultsRoot, '..', '..'), file).split('\\').join('/');
    const records = readCsvRecords(file, rel);
    const grouped = {};
    for (const r of records) {
      if (!keepSmoke && isSmoke(r.notes)) continue;
      const id = benchIdFor(r.area, r.benchmark, knownIds);
      const e = normalizeRow(r, machine);
      ((grouped[id] ??= {})[e.variant] ??= []).push(e);
    }
    const byId = {};
    for (const [id, variants] of Object.entries(grouped)) {
      byId[id] = {};
      for (const [variant, entries] of Object.entries(variants)) {
        const labels = labelEntries(entries);
        byId[id][variant] = Object.fromEntries(entries.map((e, i) => [labels[i], { label: labels[i], ...e }]));
      }
    }
    results[machine] = byId;
    machines.push({
      name: machine,
      label: machineLabel(machine),
      run: chosen,
      runs,
      date: runDate(chosen, records),
      env: readEnv(join(mdir, chosen, 'env.csv')),
    });
  }
  return { machines, results };
}

/**
 * `bench.sh --list` output (`<area>/<name>  [backend]` per line) -> catalog
 * ids. The hook for the registered-id list: save the output as
 * bench/registered-ids.txt (or point BENCH_REGISTERED_IDS at it).
 */
export function parseRegisteredList(text) {
  const ids = [];
  for (const raw of String(text).split(/\r?\n/)) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) continue;
    const path = line.split(/\s+/)[0];
    const parts = path.split('/');
    if (parts.length < 2 || parts.some((p) => !p)) continue;
    // `<package>/<slug>` is already a catalog id (no old-area mapping: math/
    // is a package, not the old flat area); the pre-0.1.5 `<area>/<slug>/<slug>`
    // form collapses its doubled slug.
    const id = parts.length === 3 && parts[1] === parts[2] ? `${parts[0]}/${parts[1]}` : path;
    if (!ids.includes(id)) ids.push(id);
  }
  return ids;
}

/**
 * The ids a block may name. Until the harness's registered list exists:
 * every id some page names, plus every id in the results. With a registered
 * list: the registered ids and the result ids, plus the page ids of packages
 * the list does not cover (the ecosystem libraries register in their repos),
 * plus `ecosystem`: ids another repo's bench/ registers that sit on a page of a
 * package the list does cover (bench/ecosystem-ids.txt; cajeta-codec's
 * deflate rows on codec/Deflate.md).
 */
export function knownIdSet({ pageIds = [], results = {}, registered = null, ecosystem = [] }) {
  const resultIds = Object.values(results).flatMap((byId) => Object.keys(byId));
  if (!registered) return new Set([...pageIds, ...resultIds]);
  const covered = new Set(registered.map((id) => id.split('/')[0]));
  return new Set([...registered, ...ecosystem, ...resultIds, ...pageIds.filter((id) => !covered.has(id.split('/')[0]))]);
}

/** bench/ecosystem-ids.txt: one `<package>/<slug>  <repo>` per line, `#` comments. */
export function parseEcosystemList(text) {
  return String(text)
    .split(/\r?\n/)
    .map((l) => l.trim())
    .filter((l) => l && !l.startsWith('#'))
    .map((l) => l.split(/\s+/)[0]);
}

/** Every fenced bench block in a markdown file, parsed. Throws naming the page. */
export function scanBlocks(markdown, srcPath) {
  const blocks = [];
  const lines = String(markdown).split(/\r?\n/);
  for (let i = 0; i < lines.length; i++) {
    const open = /^\s*(```+|~~~+)\s*([a-z-]+)\s*$/.exec(lines[i]);
    if (!open) {
      if (/^\s*(```|~~~)/.test(lines[i])) {
        // skip any other fence whole
        const fence = lines[i].trim().match(/^(`+|~+)/)[1];
        for (i++; i < lines.length && !lines[i].trim().startsWith(fence); i++);
      }
      continue;
    }
    const kind = blockKindFor(open[2]);
    const body = [];
    for (i++; i < lines.length && !lines[i].trim().startsWith(open[1]); i++) body.push(lines[i]);
    if (!kind) continue;
    try {
      blocks.push(parseBlock(kind, body.join('\n')));
    } catch (err) {
      throw new Error(`${srcPath}: ${err.message}`);
    }
  }
  return blocks;
}

function* mdFiles(dir, skip) {
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    if (entry.name.startsWith('.')) continue;
    const full = join(dir, entry.name);
    if (entry.isDirectory()) {
      if (!skip(full)) yield* mdFiles(full, skip);
    } else if (/\.md$/i.test(entry.name)) yield full;
  }
}

/** Package order from the landing's links (`](collection/README.md)`). */
function packageOrder(benchRoot) {
  const readme = join(benchRoot, 'README.md');
  if (!existsSync(readme)) return [];
  return [...readFileSync(readme, 'utf8').matchAll(/\]\(([a-z0-9_-]+)\/README\.md\)/gi)].map((m) => m[1]);
}

/**
 * Everything the site needs from bench/: the results, the blocks of every
 * page (keyed by the page's manifest path, 'benchmarks/…'), each id's metric
 * and page, the package order, and the known-id set.
 */
export function buildBenchData(benchRoot, { env = process.env } = {}) {
  if (!existsSync(benchRoot)) return { machines: [], results: {}, pages: {}, idMeta: {}, packages: [], knownIds: [] };
  const skip = (p) => {
    const rel = relative(benchRoot, p).split('\\').join('/');
    return !rel.includes('/') && BENCH_NON_CATALOG_DIRS.includes(rel);
  };
  const pages = {};
  const idMeta = {};
  for (const file of mdFiles(benchRoot, skip)) {
    const rel = relative(benchRoot, file).split('\\').join('/');
    const blocks = scanBlocks(readFileSync(file, 'utf8'), `bench/${rel}`);
    if (!blocks.length) continue;
    pages[`benchmarks/${rel}`] = { blocks };
    for (const b of blocks) {
      for (const id of blockIds(b)) {
        if (b.kind === 'chart' && idMeta[id] && idMeta[id].page !== `bench/${rel}`) {
          throw new Error(`bench/${rel}: benchmark id "${id}" is already charted on ${idMeta[id].page}`);
        }
        idMeta[id] ??= { metric: b.kind === 'chart' ? b.metric : 'throughput', page: `bench/${rel}` };
      }
    }
  }
  const pageIds = Object.keys(idMeta);
  const registeredFile = env.BENCH_REGISTERED_IDS || join(benchRoot, 'registered-ids.txt');
  const registered = existsSync(registeredFile) ? parseRegisteredList(readFileSync(registeredFile, 'utf8')) : null;
  const ecosystemFile = env.BENCH_ECOSYSTEM_IDS || join(benchRoot, 'ecosystem-ids.txt');
  const ecosystem = existsSync(ecosystemFile) ? parseEcosystemList(readFileSync(ecosystemFile, 'utf8')) : [];
  const { machines, results } = loadResults(join(benchRoot, 'results'), {
    knownIds: new Set([...pageIds, ...(registered ?? []), ...ecosystem]),
    keepSmoke: env.BENCH_KEEP_SMOKE === '1',
  });
  const knownIds = [...knownIdSet({ pageIds, results, registered, ecosystem })].sort();
  return { machines, results, pages, idMeta, packages: packageOrder(benchRoot), knownIds, registered: registered !== null };
}

