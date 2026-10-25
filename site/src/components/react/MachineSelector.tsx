import { Segmented } from './BenchChart.tsx';

export interface MachineOption {
  name: string;
  label: string;
  run: string | null;
  date: string;
  disabled: boolean;
}

/**
 * The machine selector at the top of every Benchmarks page (spec §5.4.1):
 * the machines with a run under bench/results/, or Phoenix and Proton
 * disabled with "no runs yet" before the first run. The choice goes to the
 * URL (?machine=) and is remembered per viewer; BenchPage owns that state.
 */
export default function MachineSelector({
  machines,
  value,
  onChange,
}: {
  machines: MachineOption[];
  value: string;
  onChange: (m: string) => void;
}) {
  const none = machines.every((m) => m.disabled);
  const current = machines.find((m) => m.name === value);
  return (
    <div className="bench-machine">
      <span className="bench-machine-label" id="bench-machine-label">
        Machine
      </span>
      <Segmented
        label="Machine"
        options={machines.map((m) => ({ value: m.name, label: m.label, disabled: m.disabled }))}
        value={none ? '' : value}
        onChange={onChange}
      />
      <span className="bench-machine-run">
        {none ? 'no runs yet' : current?.run ? `run ${current.run}` : 'no run on this machine'}
      </span>
    </div>
  );
}
