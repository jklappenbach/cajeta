# run-config-ergonomics — spec

## 1. Definition

### 1.1 Purpose
Make the Cajeta debug run configuration usable without the developer typing
coordinates the project already declares. Today every field is free text with no
default, so a correct configuration requires knowing the fully-qualified entry
method and the source root by heart, and a typo surfaces only as a failed launch.

### 1.2 Scope
`CajetaRunConfiguration` and its editor — the `cajeta dap` launch path. Four
changes:

- entry-method discovery and selection (§2)
- source-root defaulting (§3)
- environment variables (§4)
- `stopOnEntry`, which is currently inert (§5)

### 1.3 Current behaviour
Verified against source, not assumed:

- `CajetaRunConfigurationOptions` persists exactly three values: `entryMethod`,
  `sourceRoot`, `stopOnEntry`. There is no environment state.
- `CajetaRunConfigurationEditor` renders two bare `JBTextField`s and a checkbox.
  Neither field carries a default or a completion source.
- `DapServer.cpp` (`launch`, ~224-236) reads `entry-method` or `entryMethod`, and
  `sourceRoot` or `source-root`. It normalizes a single `::` to `.`. It reads no
  environment.
- `stopOnEntry` is persisted, and sent by `CajetaDebugSession.kt:195`, but the
  string does not occur anywhere under `src/`. The compiler never reads it, so
  the checkbox has no effect.
- The debug target runs JIT **in-process** in the `cajeta dap` server
  (`jit::startDebugSession`). It is not a spawned child process.
- `System.env` is a compiler intrinsic (`MethodCallExpression.cpp`,
  `detectSystemNamespaceReceiver`), lowered to a libc `getenv`/`setenv` wrapper
  (`runtime/native/cajeta_rt_lang.c:291`). A Cajeta program can already read its
  environment; there is no `System.cajeta` class.
- `cajeta.json` already declares `settings.build.entry-method` and
  `settings.build.source-root`. `Manifest.h` additionally carries a named-binary
  registry (`SettingsBuild.binaries`, each a `BinarySpec` with its own
  `entryMethod`).
- The xref index keys declarations by simple name (`name:<simpleName>`), exposed
  as `XrefQuery.fqnsForSimpleName`. A real shard record carries what selection
  needs: `"fqn":"mcp.Server.main"`, `"kind":"method"`,
  `"modifiers":["public","static"]`.

### 1.4 Constraints
1.4.1 Defaults are suggestions. A developer must always be able to override any
prefilled value, including typing an entry method the index has never seen.

1.4.2 The index may be absent, empty, or stale. Nothing in this feature may
present a stale or empty index as fact — see 1.4.3.

1.4.3 Degradation is visible and reasoned, matching the existing freshness
surface (`CajetaXrefFreshness`). An empty entry list must be distinguishable from
"this project has no entry methods."

1.4.4 The manifest is authoritative where it speaks. Discovery never overrides a
declared value; it only fills gaps and offers alternatives.

1.4.5 Entry-method spelling differs by source: the manifest writes
`mcp.Server::main`, the DAP wants `mcp.Server.main`. Normalization must happen in
one place, and what is persisted must be stable regardless of which source it
came from.

1.4.6 The environment applies to one launch. Because the JIT runs in-process,
mutating the server process must not leak into a later session with different
settings.

### 1.5 Non-goals
1.5.1 `CajetaTaskRunConfiguration` (the buildtool task path) is out of scope.

1.5.2 No new capture, index key, or xref schema change. §2 uses the index
exactly as it stands.

1.5.3 Editing `cajeta.json` from the run configuration UI.

1.5.4 Program arguments (argv). Related, but a separate concern from environment.

---

## 2. Entry-method discovery and selection

The entry method becomes a selectable, editable dropdown populated from declared
and discovered sources, in precedence order: manifest, then index, then nothing.

### 2.1 Requirements
2.1.1 The field is an **editable** combo box. Free text remains valid and is
persisted verbatim.

2.1.2 Candidates are gathered from, in order:
  a. `cajeta.json` — `settings.build.entry-method`, and every
     `settings.build.binaries[].entryMethod`.
  b. The xref index — declarations where `kind == "method"`, `modifiers`
     contains `static`, and the simple name is `main`.

2.1.3 Candidates are de-duplicated after normalization (1.4.5), so a method
declared in the manifest and also found in the index appears once.

2.1.4 Manifest-declared candidates sort first and are visually distinguishable
from index-discovered ones.

2.1.5 The persisted value is the normalized dotted form (`mcp.Server.main`).

### 2.2 Use cases
2.2.1 As a developer creating a debug configuration in a project whose
`cajeta.json` declares `entry-method`, when I open the configuration, then that
method is preselected and I can launch without typing anything.

2.2.2 As a developer on a multi-binary project, when I open the entry-method
dropdown, then every binary's `entryMethod` from the manifest registry is listed,
and selecting one sets the configuration to it.

2.2.3 As a developer on a project with a `main` the manifest does not declare,
when I open the dropdown, then that method appears as an index-discovered
candidate below the declared ones.

2.2.4 As a developer whose entry method is not in the manifest and not yet
indexed, when I type it by hand, then it is accepted and persisted unchanged.

2.2.5 As a developer in a project that has never been indexed, when I open the
dropdown, then it does not silently appear empty — the UI states that the index
is unavailable and offers the rebuild action, per 1.4.3.

2.2.6 As a developer who selects a manifest entry written `mcp.Server::main`,
when the configuration is saved and launched, then the DAP receives
`mcp.Server.main` and the launch resolves.

---

## 3. Source-root defaulting

### 3.1 Requirements
3.1.1 An empty `sourceRoot` resolves at configuration-creation time to, in order:
  a. `settings.build.source-root` from `cajeta.json`, resolved against the
     project base.
  b. `<projectBase>/src/main/cajeta` when that directory exists.
  c. `<projectBase>`.

3.1.2 The resolved value is written into the field as an editable default, not
applied invisibly at launch. The developer can see and change what will be used.

3.1.3 The default agrees with what `CajetaXrefRebuildAction` uses for the
whole-root export, so the run configuration and the index describe the same tree.

### 3.2 Use cases
3.2.1 As a developer creating a configuration in a project with a manifest
`source-root`, when the editor opens, then the field is prefilled with that path
resolved against the project base.

3.2.2 As a developer in a conventional project with no manifest `source-root`,
when the editor opens, then the field is prefilled with `src/main/cajeta`.

3.2.3 As a developer in a flat project with neither, when the editor opens, then
the field is prefilled with the project base rather than left empty.

3.2.4 As a developer who has set a source root deliberately, when I reopen the
configuration, then my value is preserved and no default overwrites it.

---

## 4. Environment variables

### 4.1 Requirements
4.1.1 The configuration gains environment state: a name/value map plus a flag for
whether the system environment is inherited. IntelliJ's standard
`EnvironmentVariablesComponent` provides both and is the intended widget.

4.1.2 The launch request carries the environment to the DAP server as the
standard DAP `env` field, alongside the existing `entryMethod` / `sourceRoot`.

4.1.3 `DapServer` applies the received environment before starting the debug
session, such that `System.env.get(name)` inside the debuggee observes it.

4.1.4 Because the JIT is in-process (1.4.6), the server must restore the
environment it mutated when the session ends, so a subsequent launch with
different settings is not contaminated by the previous one.

4.1.5 When inheritance is enabled, configuration entries overlay the inherited
environment; a configuration entry with the same name wins. When disabled, only
the configuration entries are visible.

4.1.6 A configuration with no environment entries and inheritance enabled
produces launch behaviour identical to today's.

### 4.2 Use cases
4.2.1 As a developer debugging code that reads `System.env.get("CAJETA_IFX_WINDOW")`,
when I set that variable in the configuration and launch, then the debuggee reads
the value I set.

4.2.2 As a developer, when I set a variable that also exists in my shell
environment, then the configuration's value wins.

4.2.3 As a developer who disables inheritance, when I launch, then variables from
my shell that I did not declare are not visible to the debuggee.

4.2.4 As a developer who launches with `FOO=1`, stops, then launches a second
configuration that does not set `FOO`, then the second run does not see `FOO` —
the first session's mutation did not persist (1.4.4 of the constraints; 4.1.4).

4.2.5 As a developer with no environment configured, when I launch, then
behaviour is unchanged from before this feature.

---

## 5. stopOnEntry

### 5.1 Requirements
5.1.1 `stopOnEntry` is honored: when set, the session suspends at the entry
method before executing its body, and reports a stop to the client.

5.1.2 When unset, execution proceeds to the first breakpoint or to completion, as
today.

5.1.3 The launch handler reads the flag from the request it already receives; the
plugin already sends it (`CajetaDebugSession.kt:195`), so no protocol change is
required.

### 5.2 Use cases
5.2.1 As a developer who ticks "stop on entry" and launches, when the session
starts, then it halts at the entry method with a usable stack and locals, before
any of the method body has run.

5.2.2 As a developer who ticks it and sets no breakpoints, when I resume, then the
program runs to completion.

5.2.3 As a developer who leaves it unticked, when I launch, then the program does
not halt at entry.

---

## 6. Cross-cutting: honesty under a cold or stale index

6.1.1 §2's index-discovered candidates depend on the xref index. §3's manifest
and convention paths do not, and §4/§5 do not.

6.1.2 A cold index degrades §2 to manifest-declared candidates plus free text.
Every other part of this feature continues to work.

6.1.3 The UI never implies the index is authoritative when it is unavailable —
"no candidates found" and "index unavailable" are different statements and must
read differently (1.4.3).

### 6.2 Use case
6.2.1 As a developer with the compiler path unset (so no index can be built),
when I open the configuration, then the manifest-declared entry methods are still
offered, the source root still defaults, and the environment and stop-on-entry
controls still work.
