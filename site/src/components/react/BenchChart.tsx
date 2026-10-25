import { useLayoutEffect, useState } from 'react';
// @ts-ignore -- plain mjs helpers shared with the rehype plugin and the build
import * as R from '../../../scripts/lib/bench-render.mjs';

/*
 * The Benchmarks charts (benchmark-reference §5), built the CorpusCharts way:
 * one measure per plot, thin bars with a 4px rounded data-end on the
 * baseline, a recessive quarter grid, direct end labels in ink tokens, a
 * per-row hover / focus tooltip, and the data table kept under the chart.
 *
 * Emphasis, not series: Cajeta's bar is --chart-mark and every competitor's
 * is --chart-context (both validated against --bg in each theme), so there is
 * no legend. The page copy says which is which.
 */

type Entry = Record<string, any>;
type Variants = Record<string, Record<string, Entry>>;

const BAR_H = 12;
const ROW_H = 26;
const FONT = 12;
const CHAR_W = 6.6; // Inter at 12px, average advance; for truncation only

function useWidth<T extends HTMLElement>(fallback = 560) {
  // a callback ref, so a plot that appears later (unmeasured -> measured on a
  // machine switch) is still observed
  const [el, setEl] = useState<T | null>(null);
  const [width, setWidth] = useState(fallback);
  useLayoutEffect(() => {
    if (!el) return;
    const set = (w: number) => setWidth(Math.max(260, Math.round(w)));
    set(el.clientWidth || fallback);
    if (typeof ResizeObserver === 'undefined') return;
    const ro = new ResizeObserver(([e]) => set(e.contentRect.width));
    ro.observe(el);
    return () => ro.disconnect();
  }, [el]);
  return [setEl, width] as const;
}

const fit = (s: string, px: number) => {
  const n = Math.max(4, Math.floor(px / CHAR_W));
  return s.length <= n ? s : s.slice(0, n - 1) + '…';
};

/** Rounded right data-end, square left edge on the baseline (CorpusCharts). */
function barPath(x0: number, w: number, y: number) {
  const r = Math.min(4, w);
  return `M ${x0} ${y} h ${w - r} a ${r} ${r} 0 0 1 ${r} ${r} v ${BAR_H - 2 * r} a ${r} ${r} 0 0 1 ${-r} ${r} h ${-(w - r)} Z`;
}

export function Unmeasured({ caption, machine }: { caption: string; machine: string }) {
  return (
    <figure className="chart bench-figure bench-unmeasured-figure">
      <figcaption>{caption}</figcaption>
      <p className="bench-unmeasured">{R.unmeasuredText(machine)}</p>
    </figure>
  );
}

export function Segmented({
  label,
  options,
  value,
  onChange,
}: {
  label: string;
  options: { value: string; label: string; disabled?: boolean }[];
  value: string;
  onChange: (v: string) => void;
}) {
  return (
    <div className="segmented" role="radiogroup" aria-label={label}>
      {options.map((o) => (
        <button
          key={o.value}
          type="button"
          role="radio"
          aria-checked={o.value === value}
          disabled={o.disabled}
          onClick={() => onChange(o.value)}
        >
          {o.label}
        </button>
      ))}
    </div>
  );
}

interface Tip {
  x: number;
  y: number;
  lines: { strong: string; rest: string; details: string[] };
}

function TipBox({ tip, width }: { tip: Tip | null; width: number }) {
  if (!tip) return null;
  return (
    <div className="chart-tip" style={{ left: Math.max(0, Math.min(tip.x + 12, width - 236)), top: tip.y + 12 }} role="status">
      <strong>{tip.lines.strong}</strong> {tip.lines.rest}
      {tip.lines.details.map((d) => (
        <span key={d} className="chart-tip-line">
          {d}
        </span>
      ))}
    </div>
  );
}

function entryDetails(e: Entry): string[] {
  const lib = R.libraryText(e);
  return [
    lib ? `${lib}${e.versions?.library ? ' ' + e.versions.library : ''}` : '',
    e.versions?.language ? `${R.languageLabel(e.language)} ${e.versions.language}` : '',
    `min ${R.formatNs(e.min)} · median ${R.formatNs(e.median)} · p95 ${R.formatNs(e.p95)}`,
    `alloc ${R.formatBytes(e.alloc)} · peak RSS ${R.MEASURES.memory.format(e.rss)}`,
    e.backend && e.backend !== 'cpu' ? `backend ${e.backend}${e.driver ? ' · ' + e.driver : ''}` : '',
    e.status === 'ok' ? e.reason : R.statusText(e),
  ].filter(Boolean);
}

function ChartTable({ model }: { model: any }) {
  return (
    <details className="bench-data">
      <summary>Data</summary>
      {model.variants.map(({ variant, rows }: any) => (
        <table key={variant}>
          {variant && <caption>{variant}</caption>}
          <thead>
            <tr>
              {R.CHART_COLUMNS.map((c: string) => (
                <th key={c}>{c}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {rows.map(({ label, entry: e }: any) => (
              <tr key={label}>
                <td>{label}</td>
                <td>{R.libraryText(e)}</td>
                <td>{R.versionText(e)}</td>
                <td>{R.formatNs(e.median)}</td>
                <td>{R.formatNs(e.min)}</td>
                <td>{R.formatNs(e.p95)}</td>
                <td>{R.formatBytes(e.alloc)}</td>
                <td>{R.MEASURES.memory.format(e.rss)}</td>
                <td>{R.notesText(e)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      ))}
    </details>
  );
}

/** A `bench` block: bars by language, best first; one small multiple per variant. */
export function BenchChart({
  id,
  metric,
  variants,
  machine,
  date,
}: {
  id: string;
  metric: string;
  variants: Variants | undefined;
  machine: string;
  date: string;
}) {
  const [measure, setMeasure] = useState<string>(R.initialMeasure(metric));
  const [tip, setTip] = useState<Tip | null>(null);
  const [plotRef, width] = useWidth<HTMLDivElement>();
  if (!variants || !Object.keys(variants).length) return <Unmeasured caption={id} machine={machine} />;

  const model = R.chartModel(variants, measure);
  const labelW = Math.min(156, Math.round(width * 0.3));
  const valueW = 86;
  const plotW = Math.max(60, width - labelW - valueW);
  const x0 = labelW;
  const scale = (v: number) => Math.max(2, (v / (model.max || 1)) * plotW);
  const multi = model.variants.length > 1;

  const show = (e: React.SyntheticEvent, row: any, y: number) => {
    const box = (e.currentTarget as SVGElement).closest('.chart-plot')!.getBoundingClientRect();
    const pointer = 'clientX' in e ? (e as React.MouseEvent) : null;
    setTip({
      x: pointer ? pointer.clientX - box.left : x0,
      y: pointer ? pointer.clientY - box.top : y,
      lines: { strong: row.text, rest: row.label, details: entryDetails(row.entry) },
    });
  };

  let offset = 0;
  return (
    <figure className="chart bench-figure">
      <figcaption>{R.captionText(id, model.better, machine, date)}</figcaption>
      <Segmented
        label="Measure"
        options={R.measuresFor(metric).map((m: string) => ({ value: m, label: (R.MEASURES as Record<string, any>)[m].label }))}
        value={measure}
        onChange={(m) => {
          setTip(null);
          setMeasure(m);
        }}
      />
      <div className="chart-plot" ref={plotRef} onMouseLeave={() => setTip(null)} style={{ position: 'relative' }}>
        {model.variants.map(({ variant, rows }: any) => {
          const head = multi ? 20 : 0;
          const height = head + rows.length * ROW_H + 4;
          const top = offset;
          offset += height;
          return (
            <svg
              key={variant}
              viewBox={`0 0 ${width} ${height}`}
              width="100%"
              role="img"
              aria-label={`${id}${variant ? ' ' + variant : ''}: ${rows.map((r: any) => `${r.label} ${r.text}`).join(', ')}`}
            >
              {multi && (
                <text x={0} y={14} fontSize={FONT} fontWeight={600} fill="var(--ink-soft)" fontFamily="var(--font-ui)">
                  {variant || 'default'}
                </text>
              )}
              {[0, 0.25, 0.5, 0.75, 1].map((t) => (
                <line
                  key={t}
                  x1={x0 + plotW * t}
                  x2={x0 + plotW * t}
                  y1={head}
                  y2={height - 4}
                  stroke="var(--line)"
                  strokeWidth={1}
                />
              ))}
              {rows.map((r: any, i: number) => {
                const y = head + i * ROW_H + (ROW_H - BAR_H) / 2;
                const measured = r.value !== null;
                const w = measured ? scale(r.value) : 0;
                const textY = y + BAR_H - 2;
                return (
                  <g
                    key={r.label}
                    className="bench-row"
                    tabIndex={0}
                    onMouseEnter={(e) => show(e, r, top + y)}
                    onMouseMove={(e) => show(e, r, top + y)}
                    onFocus={(e) => show(e, r, top + y + BAR_H)}
                    onBlur={() => setTip(null)}
                  >
                    <rect x={0} y={head + i * ROW_H} width={width} height={ROW_H} fill="transparent" />
                    <text
                      x={labelW - 10}
                      y={textY}
                      textAnchor="end"
                      fontSize={FONT}
                      fontWeight={r.entry.cajeta ? 600 : 400}
                      fill={measured ? 'var(--ink-soft)' : 'var(--ink-faint)'}
                      fontFamily="var(--font-ui)"
                    >
                      {fit(r.label, labelW - 14)}
                    </text>
                    {measured ? (
                      <>
                        <path
                          d={barPath(x0, w, y)}
                          fill={r.entry.cajeta ? 'var(--chart-mark)' : 'var(--chart-context)'}
                        />
                        <text
                          x={x0 + w + 8}
                          y={textY}
                          fontSize={FONT}
                          fontWeight={600}
                          fill="var(--ink)"
                          fontFamily="var(--font-ui)"
                        >
                          {r.text}
                        </text>
                      </>
                    ) : (
                      <text x={x0 + 6} y={textY} fontSize={FONT} fill="var(--ink-faint)" fontFamily="var(--font-ui)">
                        {fit(r.text, plotW + valueW - 12)}
                      </text>
                    )}
                  </g>
                );
              })}
            </svg>
          );
        })}
        <TipBox tip={tip} width={width} />
      </div>
      <ChartTable model={model} />
    </figure>
  );
}

// ------------------------------------------------------------------ standings

const fmtTick = (t: number) => (t >= 1 ? `${t}×` : `1/${Math.round(1 / t)}×`);

/** The landing standings: a dot per package on a log ratio axis, parity at 1. */
export function BenchStandings({
  rows,
  machine,
  date,
}: {
  rows: any[] | undefined;
  machine: string;
  date: string;
}) {
  const [tip, setTip] = useState<Tip | null>(null);
  const [plotRef, width] = useWidth<HTMLDivElement>();
  if (!rows || !rows.length) return <Unmeasured caption="Standings" machine={machine} />;

  const colW = 46;
  const labelW = Math.min(118, Math.round(width * 0.24));
  const plotW = Math.max(80, width - labelW - colW * 3 - 20);
  const x0 = labelW;
  const head = 22;
  const axis = 34;
  const height = head + rows.length * ROW_H + axis;
  const ratios = rows.map((r) => r.ratio).filter((r) => r !== null);
  const span = Math.max(Math.log2(2), ...ratios.map((r) => Math.abs(Math.log2(r)))) * 1.12;
  const x = (r: number) => x0 + ((Math.log2(r) + span) / (2 * span)) * plotW;
  // powers of two, thinned to at most seven, always through parity
  const reach = Math.floor(span);
  const step = Math.max(1, Math.ceil((2 * reach + 1) / 7));
  const ticks: number[] = [];
  for (let k = -reach - (-reach % step); k <= reach; k += step) ticks.push(2 ** k);
  const cols = ['wins', 'losses', 'skips'];
  const colX = (i: number) => x0 + plotW + 20 + colW * i + colW - 6;

  return (
    <figure className="chart bench-figure">
      <figcaption>{R.captionText(R.STANDINGS_SUBJECT, 'lower', machine, date)}</figcaption>
      <div className="chart-plot" ref={plotRef} onMouseLeave={() => setTip(null)} style={{ position: 'relative' }}>
        <svg viewBox={`0 0 ${width} ${height}`} width="100%" role="img" aria-label="Standings by package">
          {ticks.map((t) => (
            <line key={t} x1={x(t)} x2={x(t)} y1={head} y2={height - axis} stroke="var(--line)" strokeWidth={1} />
          ))}
          <line x1={x(1)} x2={x(1)} y1={head - 4} y2={height - axis} stroke="var(--ink-faint)" strokeWidth={1} />
          <text x={x(1)} y={12} textAnchor="middle" fontSize={11} fill="var(--ink-soft)" fontFamily="var(--font-ui)">
            parity
          </text>
          {cols.map((c, i) => (
            <text key={c} x={colX(i)} y={12} textAnchor="end" fontSize={11} fill="var(--ink-soft)" fontFamily="var(--font-ui)">
              {c}
            </text>
          ))}
          {rows.map((r, i) => {
            const cy = head + i * ROW_H + ROW_H / 2;
            const showTip = (e: React.SyntheticEvent) => {
              const box = (e.currentTarget as SVGElement).closest('.chart-plot')!.getBoundingClientRect();
              const p = 'clientX' in e ? (e as React.MouseEvent) : null;
              setTip({
                x: p ? p.clientX - box.left : x0,
                y: p ? p.clientY - box.top : cy,
                lines: {
                  strong: R.formatRatio(r.ratio),
                  rest: r.pkg,
                  details: [
                    `${r.n} benchmark${r.n === 1 ? '' : 's'} compared`,
                    `${r.wins} wins · ${r.losses} losses · ${r.skips} skips`,
                  ],
                },
              });
            };
            return (
              <g key={r.pkg} className="bench-row" tabIndex={0} onMouseEnter={showTip} onMouseMove={showTip} onFocus={showTip} onBlur={() => setTip(null)}>
                <rect x={0} y={cy - ROW_H / 2} width={width} height={ROW_H} fill="transparent" />
                <text x={labelW - 10} y={cy + 4} textAnchor="end" fontSize={FONT} fill="var(--ink-soft)" fontFamily="var(--font-ui)">
                  {fit(r.pkg, labelW - 14)}
                </text>
                {r.ratio !== null ? (
                  <circle cx={x(r.ratio)} cy={cy} r={5} fill="var(--chart-mark)" stroke="var(--bg-raised)" strokeWidth={2} />
                ) : (
                  <text x={x0 + 6} y={cy + 4} fontSize={FONT} fill="var(--ink-faint)" fontFamily="var(--font-ui)">
                    not compared
                  </text>
                )}
                {[r.wins, r.losses, r.skips].map((v, j) => (
                  <text key={j} x={colX(j)} y={cy + 4} textAnchor="end" fontSize={FONT} fill="var(--ink-soft)" fontFamily="var(--font-ui)" className="tabular">
                    {v}
                  </text>
                ))}
              </g>
            );
          })}
          {ticks.map((t) => (
            <text key={t} x={x(t)} y={height - axis + 14} textAnchor="middle" fontSize={11} fill="var(--ink-faint)" fontFamily="var(--font-ui)">
              {fmtTick(t)}
            </text>
          ))}
          <text x={x0} y={height - 4} fontSize={11} fill="var(--ink-faint)" fontFamily="var(--font-ui)">
            ← Cajeta faster
          </text>
          <text x={x0 + plotW} y={height - 4} textAnchor="end" fontSize={11} fill="var(--ink-faint)" fontFamily="var(--font-ui)">
            Cajeta slower →
          </text>
        </svg>
        <TipBox tip={tip} width={width} />
      </div>
      <details className="bench-data">
        <summary>Data</summary>
        <table>
          <thead>
            <tr>
              {['package', 'geomean ratio', 'wins', 'losses', 'skips'].map((h) => (
                <th key={h}>{h}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {rows.map((r) => (
              <tr key={r.pkg}>
                <td>{r.pkg}</td>
                <td>{R.formatRatio(r.ratio)}</td>
                <td>{r.wins}</td>
                <td>{r.losses}</td>
                <td>{r.skips}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </details>
    </figure>
  );
}

// ------------------------------------------------------------------ matrix

function MatrixBar({ value, max, fill, name }: { value: number | null; max: number; fill: string; name: string }) {
  return (
    <div className="bench-mbar" title={`${name}: ${R.formatTps(value)}`}>
      <span className="bench-mbar-track">
        {value !== null && (
          <span className="bench-mbar-fill" style={{ width: `${Math.max(2, (value / (max || 1)) * 100)}%`, background: fill }} />
        )}
      </span>
      <span className="bench-mbar-value">{value !== null ? R.formatTps(value) : '—'}</span>
    </div>
  );
}

/** The llm matrix: per model, rows os × driver × backend, a cell per id with both platforms. */
export function BenchMatrix({
  block,
  results,
  machine,
  date,
}: {
  block: any;
  results: Record<string, Variants> | undefined;
  machine: string;
  date: string;
}) {
  const model = R.matrixModel(results ?? {}, block);
  if (!model.models.length) return <Unmeasured caption={R.matrixSubject(block)} machine={machine} />;
  const ids = model.columns.flatMap((c: any) => c.ids.map((i: any) => ({ ...i, measure: c.measure })));
  return (
    <>
      {model.models.map((m: any) => {
        const max: Record<string, number> = {};
        for (const i of ids) {
          max[i.id] = Math.max(0, ...m.rows.flatMap((r: any) => [r.cells[i.id]?.cajeta ?? 0, r.cells[i.id]?.other ?? 0]));
        }
        return (
          <figure className="chart bench-figure" key={m.model}>
            <figcaption>{R.captionText(`${m.model} · tokens/s`, 'higher', machine, date)}</figcaption>
            <ul className="bench-legend" aria-label="Series">
              <li>
                <span className="bench-swatch" style={{ background: 'var(--chart-mark)' }} />
                {m.names.cajeta}
              </li>
              <li>
                <span className="bench-swatch" style={{ background: 'var(--chart-context)' }} />
                {m.names.other}
              </li>
            </ul>
            <div className="bench-matrix" style={{ ['--bench-cols' as any]: ids.length }} role="table" aria-label={`${m.model} tokens per second`}>
              <div className="bench-matrix-head" role="row">
                <span className="bench-matrix-key" role="columnheader">
                  {model.keys.join(' · ')}
                </span>
                {ids.map((i: any) => (
                  <span key={i.id} role="columnheader" className="bench-matrix-col">
                    <b>{i.measure}</b> {i.label !== i.measure ? i.label : ''}
                  </span>
                ))}
              </div>
              {m.rows.map((r: any) => (
                <div className="bench-matrix-row" role="row" key={r.key.join('|')}>
                  <span className="bench-matrix-key" role="rowheader">
                    {r.key.join(' · ')}
                  </span>
                  {ids.map((i: any) => {
                    const c = r.cells[i.id] ?? { cajeta: null, other: null, ratio: null };
                    return (
                      <span className="bench-matrix-cell" role="cell" key={i.id}>
                        <span className="bench-matrix-cell-label">
                          {i.measure} {i.label !== i.measure ? i.label : ''}
                        </span>
                        <MatrixBar value={c.cajeta} max={max[i.id]} fill="var(--chart-mark)" name={m.names.cajeta} />
                        <MatrixBar value={c.other} max={max[i.id]} fill="var(--chart-context)" name={m.names.other} />
                        <span className="bench-ratio">{R.formatRatio(c.ratio)}</span>
                      </span>
                    );
                  })}
                </div>
              ))}
            </div>
            <details className="bench-data">
              <summary>Data</summary>
              <div dangerouslySetInnerHTML={{ __html: R.matrixTableHtml(model, m) }} />
            </details>
          </figure>
        );
      })}
    </>
  );
}
