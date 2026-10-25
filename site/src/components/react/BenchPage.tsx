import { useEffect, useState } from 'react';
import { createPortal } from 'react-dom';
// @ts-ignore -- plain mjs helpers shared with the rehype plugin and the build
import { blockFromAttrs } from '../../../scripts/lib/bench-render.mjs';
import MachineSelector, { type MachineOption } from './MachineSelector.tsx';
import { BenchChart, BenchMatrix, BenchStandings } from './BenchChart.tsx';

/*
 * The one island on a Benchmarks page (benchmark-reference §5). It renders
 * the machine selector where it is mounted (top of the page) and, on
 * hydrate, finds every <bench-chart> placeholder rehype-bench left in the
 * page, clears its static fallback and portals the chart into it. Its props
 * carry the page's rows for every machine, so a machine switch re-renders
 * every chart without a page load.
 */

export interface BenchPageProps {
  pagePath: string;
  machines: MachineOption[];
  defaultMachine: string;
  results: Record<string, Record<string, any>>;
  standings: Record<string, any[]>;
}

const STORE_KEY = 'cajeta-bench-machine';

function readStored(): string | null {
  try {
    return localStorage.getItem(STORE_KEY);
  } catch {
    return null;
  }
}

function writeStored(m: string) {
  try {
    localStorage.setItem(STORE_KEY, m);
  } catch {
    /* private window or blocked storage: the URL still carries the choice */
  }
}

interface Target {
  el: Element;
  block: any;
}

export default function BenchPage(props: BenchPageProps) {
  const [machine, setMachine] = useState(props.defaultMachine);
  const [targets, setTargets] = useState<Target[]>([]);

  useEffect(() => {
    const usable = (m: string | null) => !!m && props.machines.some((x) => x.name === m && !x.disabled);
    const fromUrl = new URLSearchParams(location.search).get('machine');
    const stored = readStored();
    setMachine(usable(fromUrl) ? fromUrl! : usable(stored) ? stored! : props.defaultMachine);
    const found = [...document.querySelectorAll('bench-chart')].map((el) => {
      const block = blockFromAttrs((k: string) => el.getAttribute(`data-${k}`));
      el.replaceChildren();
      return { el, block };
    });
    setTargets(found);
  }, []);

  const choose = (m: string) => {
    setMachine(m);
    writeStored(m);
    try {
      const url = new URL(location.href);
      url.searchParams.set('machine', m);
      history.replaceState(history.state, '', url);
    } catch {
      /* non-browser host */
    }
  };

  const date = props.machines.find((m) => m.name === machine)?.date ?? '';
  const results = props.results[machine] ?? {};

  return (
    <>
      <MachineSelector machines={props.machines} value={machine} onChange={choose} />
      {targets.map(({ el, block }, i) =>
        createPortal(
          block.kind === 'chart' ? (
            <BenchChart id={block.id} metric={block.metric} variants={results[block.id]} machine={machine} date={date} />
          ) : block.kind === 'standings' ? (
            <BenchStandings rows={props.standings[machine]} machine={machine} date={date} />
          ) : (
            <BenchMatrix block={block} results={results} machine={machine} date={date} />
          ),
          el,
          `bench-${i}`
        )
      )}
    </>
  );
}
