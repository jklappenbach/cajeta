import { useMemo, useState } from 'react';
// @ts-ignore -- shared plain-js helper, unit-tested under scripts/lib
import { filterMatch } from '../../../scripts/lib/extract.mjs';

export interface FilterDoc {
  url: string;
  title: string;
  description: string;
  section: string;
  sectionLabel: string;
  minutes: number;
}

/** Live filter over the doc corpus; renders its own result cards. */
export default function DocFilter({
  docs,
  placeholder = 'Filter documents…',
}: {
  docs: FilterDoc[];
  placeholder?: string;
}) {
  const [query, setQuery] = useState('');

  const hits = useMemo(
    () => (query.trim() ? docs.filter((d) => filterMatch(d, query)) : null),
    [docs, query]
  );

  return (
    <div className="doc-filter">
      <input
        type="search"
        value={query}
        placeholder={placeholder}
        aria-label={placeholder}
        onChange={(e) => setQuery(e.target.value)}
      />
      {hits !== null && (
        <>
          <p className="count">
            {hits.length} of {docs.length} documents
          </p>
          {hits.length === 0 ? (
            <p className="empty">Nothing matches — try fewer terms.</p>
          ) : (
            <div className="card-grid">
              {hits.slice(0, 60).map((d) => (
                <a key={d.url} className="doc-card" href={d.url}>
                  <h3>{d.title}</h3>
                  <p>{d.description}</p>
                  <span className="meta">
                    {d.sectionLabel} · {d.minutes} min
                  </span>
                </a>
              ))}
            </div>
          )}
        </>
      )}
    </div>
  );
}
