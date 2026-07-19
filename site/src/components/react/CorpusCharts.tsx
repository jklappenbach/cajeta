import { useState } from 'react';

interface Row {
  label: string;
  value: number;
  detail: string;
  url?: string;
}

interface TooltipState {
  x: number;
  y: number;
  row: Row;
}

/**
 * Horizontal bar chart, single measure -> single hue (--chart-mark, validated
 * for both themes). Thin bars, rounded data-end anchored at the baseline,
 * direct end-labels in ink tokens, per-mark hover tooltip, table fallback.
 */
function BarChart({ title, unit, rows }: { title: string; unit: string; rows: Row[] }) {
  const [tip, setTip] = useState<TooltipState | null>(null);

  const labelW = 118;
  const valueW = 52;
  const rowH = 30;
  const barH = 12;
  const width = 460;
  const plotW = width - labelW - valueW;
  const height = rows.length * rowH + 8;
  const max = Math.max(...rows.map((r) => r.value), 1);

  const barPath = (w: number, y: number) => {
    const r = Math.min(4, w);
    // rounded right data-end, square left edge on the baseline
    return `M ${labelW} ${y} h ${w - r} a ${r} ${r} 0 0 1 ${r} ${r} v ${barH - 2 * r} a ${r} ${r} 0 0 1 ${-r} ${r} h ${-(w - r)} Z`;
  };

  return (
    <figure className="chart">
      <figcaption>{title}</figcaption>
      <div
        className="chart-plot"
        onMouseLeave={() => setTip(null)}
        style={{ position: 'relative' }}
      >
        <svg viewBox={`0 0 ${width} ${height}`} role="img" aria-label={title}>
          {/* recessive grid: quarter ticks */}
          {[0.25, 0.5, 0.75, 1].map((t) => (
            <line
              key={t}
              x1={labelW + plotW * t}
              x2={labelW + plotW * t}
              y1={0}
              y2={height - 8}
              stroke="var(--line)"
              strokeWidth={1}
            />
          ))}
          {rows.map((r, i) => {
            const y = i * rowH + (rowH - barH) / 2;
            const w = Math.max(2, (r.value / max) * plotW);
            return (
              <g
                key={r.label}
                onMouseEnter={(e) => {
                  const box = (e.currentTarget.ownerSVGElement as SVGSVGElement)
                    .closest('.chart-plot')!
                    .getBoundingClientRect();
                  setTip({ x: e.clientX - box.left, y: e.clientY - box.top, row: r });
                }}
                style={{ cursor: r.url ? 'pointer' : 'default' }}
                onClick={() => r.url && (location.href = r.url)}
              >
                {/* hover hit target: the whole row */}
                <rect x={0} y={i * rowH} width={width} height={rowH} fill="transparent" />
                <text
                  x={labelW - 10}
                  y={y + barH - 2}
                  textAnchor="end"
                  fontSize={12}
                  fill="var(--ink-soft)"
                  fontFamily="var(--font-ui)"
                >
                  {r.label}
                </text>
                <path d={barPath(w, y)} fill="var(--chart-mark)" />
                <text
                  x={labelW + w + 8}
                  y={y + barH - 2}
                  fontSize={12}
                  fontWeight={600}
                  fill="var(--ink)"
                  fontFamily="var(--font-ui)"
                >
                  {r.value.toLocaleString()}
                </text>
              </g>
            );
          })}
        </svg>
        {tip && (
          <div
            className="chart-tip"
            style={{ left: Math.min(tip.x + 12, 320), top: tip.y + 12 }}
            role="status"
          >
            <strong>{tip.row.label}</strong>
            <br />
            {tip.row.value.toLocaleString()} {unit} · {tip.row.detail}
          </div>
        )}
      </div>
      <details>
        <summary>View as table</summary>
        <table>
          <thead>
            <tr>
              <th>{title.split('·')[0]}</th>
              <th>{unit}</th>
              <th>detail</th>
            </tr>
          </thead>
          <tbody>
            {rows.map((r) => (
              <tr key={r.label}>
                <td>{r.label}</td>
                <td>{r.value.toLocaleString()}</td>
                <td>{r.detail}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </details>
    </figure>
  );
}

export interface SectionStat {
  label: string;
  url: string;
  count: number;
  words: number;
}

export default function CorpusCharts({
  sections,
  minuteBuckets,
}: {
  sections: SectionStat[];
  minuteBuckets: { label: string; count: number; share: number }[];
}) {
  const byCount = [...sections].sort((a, b) => b.count - a.count);
  return (
    <div className="chart-row">
      <BarChart
        title="Documents per section"
        unit="documents"
        rows={byCount.map((s) => ({
          label: s.label,
          value: s.count,
          detail: `${Math.round(s.words / 1000)}k words`,
          url: s.url,
        }))}
      />
      <BarChart
        title="Reading-length distribution"
        unit="documents"
        rows={minuteBuckets.map((b) => ({
          label: b.label,
          value: b.count,
          detail: `${b.share}% of corpus`,
        }))}
      />
    </div>
  );
}
