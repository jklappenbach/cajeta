# profile-run-history — every profiled run keeps its own trace

## 1. Definition

### 1.1 Purpose
A profiled run must leave a trace that no later run overwrites, in a location
that survives a clean, named so that runs order by time without consulting a
second source of truth. The profiler tool window must find those runs and let
one be chosen.

### 1.2 The problem — measured
The runtime writes ONE file and overwrites it. `__cajeta_prof_out_path()`
returns `CAJETA_PROFILER_OUT`, or the literal `cajeta.pftrace` in the working
directory when that is unset. Profile twice and the first run is gone. The
common reason to profile twice — did the change help? — is exactly the case the
current default destroys.

**The capability is half-built, and its tests hid that.**
`CajetaProfileLocation.traceFile(project, configurationName, stamp)` already
composes `build/cajeta/profiles/<config>-<stamp>.pftrace`, and its own comment
gives this spec's rationale: *"consecutive runs of the same configuration do not
overwrite each other."* `mostRecent(project)` already searches for the newest
trace. **Neither has a caller.** `CajetaProfileLocationTest` exercises both, so
they are green, documented, and wired to nothing; nothing has ever written into
`build/cajeta/profiles`. Only `defaultDirectory`, `isArmed` and `OUT` are used
in production.

That directory is also the wrong home: `clean` removes `build/` wholesale, so
the history would not survive the one operation it most needs to.

### 1.3 A profile is a record, not an artifact or a cache
This is the constraint the rest follows from. An artifact is reproducible from
source. A cache is reproducible by definition. **A profile is a measurement of a
moment that no longer exists** — rebuild the binary and it cannot be recovered.
So it does not belong under `build/`, it is not purged with the cache, and
deleting it is never a side effect of another verb.

### 1.4 Constraints
- **Survives `clean`.** `clean` removes `build/` and `.cajeta/cache/`
  (`keep-cache: true` spares the second). Storage must fall outside both.
- **Never committed.** `.cajeta/` is gitignored (`.gitignore:114`), per project.
- **Ordering without a sidecar.** A `runs.json` beside the traces would be a
  second source of truth for what a directory contains. Order is derived from
  the names.
- **No new capture format.** A run already emits one `.pftrace`. This is
  storage, naming and selection.

### 1.5 Non-goals
- **Comparison between runs is out of scope.** Dropped deliberately
  2026-08-31. A delta view has to answer whether a difference exceeds its own
  noise before it reports one, and that is a separate body of work.
- Remote or shared storage. Runs are local to a checkout.
- Changing what a trace contains.

---

## 2. Where runs are stored

Runs live in **`.cajeta/profiles/`** under the project root: outside `build/`,
outside `.cajeta/cache/`, gitignored, and beside the code the measurement
describes.

### Use cases
- **2.1** When a profiled run completes, its trace is written under
  `.cajeta/profiles/`.
- **2.2** When `clean` runs, profiles are untouched.
- **2.3** When `clean` runs with `keep-cache: true`, profiles are untouched.
- **2.4** When the cache is purged (`.cajeta/cache/`), profiles are untouched.
- **2.5** When the directory does not exist, the first profiled run creates it.
- **2.6** When the directory cannot be created or written, the run says so on
  stderr and still completes — profiling never fails the program being profiled.
- **2.7** When a project is committed, no profile is included.

---

## 3. Naming and ordering

One file per run: `<stamp>-<label>.pftrace`, where `<stamp>` is **ISO-8601 basic
UTC** (`20260831T201530Z`) and `<label>` names what produced the run (a run
configuration, an entry method, or `run` when nothing better is known).

The stamp leads so lexicographic order IS chronological order — a directory
listing is already sorted, with no index to consult and nothing to disagree
with. Basic UTC because local time does not order across a DST boundary and a
bare epoch does not read in a chooser.

One file, not a directory per run: a run emits exactly one artifact today, and a
directory would be ceremony. Revisit if a run starts emitting siblings.

### Use cases
- **3.1** When two runs occur, each writes its own file and neither overwrites
  the other.
- **3.2** When runs are listed by filename, they are in chronological order.
- **3.3** When two runs land within the same second, both are kept — the second
  is disambiguated rather than overwriting the first.
- **3.4** When a label contains characters a filesystem rejects, it is
  sanitized, and never to the empty string.
- **3.5** When the stamp is read back, it identifies the run's wall-clock start
  in UTC.

---

## 4. What produces a run

Three paths must all land in the same place, because a developer who profiles
from a shell and then reaches for the IDE should not have to explain where the
file is.

**The CLI default changes.** With `CAJETA_PROFILER_OUT` unset, the runtime
writes into `.cajeta/profiles/` under the project root instead of
`./cajeta.pftrace`. This is a deliberate behaviour change: the old default
silently overwrote the previous run, which is the defect. Anything scripted
against the literal `cajeta.pftrace` must set `CAJETA_PROFILER_OUT`, and that
belongs in the release notes rather than arriving as a surprise.

### Use cases
- **4.1** When `CAJETA_PROFILER=1` is set and `CAJETA_PROFILER_OUT` is not, the
  trace is written into `.cajeta/profiles/` with a stamped name.
- **4.2** When `CAJETA_PROFILER_OUT` names a path, it is honoured exactly — an
  explicit choice is never redirected.
- **4.3** When a run is started from the IDE with profiling armed, its trace
  lands in the same directory as a shell run's, under the same naming.
- **4.4** When no project root can be determined, the run falls back to the
  working directory and says where it wrote.

---

## 5. Discovery and selection

The tool window lists the runs it finds and lets one be chosen. Today it has no
listing at all: a trace is opened through **Tools → Cajeta → Open Cajeta
Profile…**, a file chooser, and the window offers nothing when empty.

### Use cases
- **5.1** When the profiler tool window opens, the runs under
  `.cajeta/profiles/` are listed, newest first.
- **5.2** When a run is chosen from the list, its trace is loaded and displayed.
- **5.3** When a new run completes while the window is open, it appears in the
  list without a reload.
- **5.4** When no runs exist, the window says so and states how one is produced,
  rather than showing an empty panel.
- **5.5** When a listed file is unreadable or not a trace, it is reported as
  such and the rest of the list still works.
- **5.6** When a trace outside the directory is wanted, the existing Open action
  still opens it.

---

## 6. Removal and retention

Deleting measurements is deliberate, never a side effect.

`clean` gains **`profiles: true`** — opt-in, matching the existing `keep-cache`
parameter style.

**On the `clean` precedent.** `CleanAction`'s header records that making the
cache wipe opt-IN was a bug: a non-interactive Clean answered "no" at EOF, left
the artifact cache, and the next build re-published a cached binary in ~100 ms,
presenting as an instant green check under an empty phase tree. That precedent
does not transfer. The failure there was that leftover state made a subsequent
build **lie**. Leftover profiles make nothing lie; they occupy disk. The cost is
asymmetric in the other direction — a stale cache wastes minutes, a deleted
measurement is unrecoverable — so opt-in is correct here for the same reason
opt-out was correct there.

**Retention: a count cap, default 50.** It bounds disk without ever deleting the
run someone was about to open. This is the one number worth arguing about; an
age cap was considered and rejected because "old" is not what makes a profile
uninteresting.

### Use cases
- **6.1** When `clean` runs without `profiles: true`, no profile is removed.
- **6.2** When `clean` runs with `profiles: true`, every profile is removed and
  the count and bytes are reported.
- **6.3** When a new run would exceed the retention cap, the oldest runs are
  removed until the cap holds, and what was removed is reported.
- **6.4** When the cap is configured in the manifest, that value is used.
- **6.5** When the cap is configured as unlimited, nothing is ever pruned
  automatically.

---

## 7. Migration and compatibility

- **7.1** When an existing `./cajeta.pftrace` is present, it is left alone —
  nothing moves or deletes a file written by an older toolchain.
- **7.2** When a trace written by an older toolchain is opened, it loads: the
  format is unchanged and this spec changes only where files live and what they
  are called.
- **7.3** When `CAJETA_PROFILER_OUT` is set, behaviour is exactly as before.
