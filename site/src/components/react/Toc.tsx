import { useEffect, useState } from 'react';

export interface TocHeading {
  depth: number;
  slug: string;
  text: string;
}

/** Scroll-tracking table of contents for a rendered doc page. */
export default function Toc({ headings }: { headings: TocHeading[] }) {
  const items = headings.filter((h) => h.depth === 2 || h.depth === 3);
  const [active, setActive] = useState<string>('');

  useEffect(() => {
    const targets = items
      .map((h) => document.getElementById(h.slug))
      .filter((el): el is HTMLElement => el !== null);
    if (targets.length === 0) return;

    const observer = new IntersectionObserver(
      (entries) => {
        for (const entry of entries) {
          if (entry.isIntersecting) {
            setActive(entry.target.id);
            break;
          }
        }
      },
      { rootMargin: '-10% 0px -75% 0px' }
    );
    targets.forEach((t) => observer.observe(t));
    return () => observer.disconnect();
  }, []);

  if (items.length < 2) return null;

  return (
    <div>
      <p className="toc-title">On this page</p>
      <ul>
        {items.map((h) => (
          <li key={h.slug}>
            <a
              href={`#${h.slug}`}
              className={`depth-${h.depth}${active === h.slug ? ' active' : ''}`}
            >
              {h.text}
            </a>
          </li>
        ))}
      </ul>
    </div>
  );
}
