# A short type name imported by two packages binds to the alphabetically
# first one, silently, across the whole merged module

**Filed 2026-08-20** (found by cajeta-llm Unit 15 when
`cajeta.xpu.Capability` became the stdlib's first real short-name
collision, against `cajeta.nucleo.frame.Capability`).

## Symptom

A 7-line program that imports one unrelated class and does nothing:

```cajeta
package probe;
import cajeta.nucleo.frame.DynFrame;
public final class R {
    public static int32 run() { return 1; }
}
```

fails to build, inside the STDLIB, at `cajeta.math.Ewise:1117`:

```
CAJETA_ERROR_NO_MATCHING_OVERLOAD: no overload of 'supports' on
'cajeta.xpu.Device' accepts 1 argument(s). Candidates:
    supports(cajeta.xpu.Capability)   (method=matmulBf16Op)
```

The candidate list is the giveaway: the sole candidate takes exactly one
argument, so the arity in the message is not the real complaint — the
ARGUMENT TYPE is. `CAJETA_DBG_RESOLVE=supports` shows it outright:

```
[dbg-res] resolve 'supports' on cajeta.xpu.Device floating=0
[dbg-res]   call canonical: cajeta.xpu.Device::supports(cajeta.nucleo.frame.Capability)
[dbg-res]   arg label='' type=cajeta.nucleo.frame.Capability
[dbg-res]     canonical: cajeta.xpu.Device::supports(cajeta.xpu.Capability)
```

`Ewise.cajeta` contains `import cajeta.xpu.Capability;` and no reference
whatsoever to `cajeta.nucleo.frame`. Its `Capability` still bound to
nucleo's.

## Cause

Imports are recorded per MODULE, not per compilation unit:

```cpp
// CajetaModule::onImportDeclaration
imports[qName->getTypeName()][qName->getPackageName()] = qName;
```

Nothing ever clears this map, and the stdlib is compiled as one merged
module, so every file's imports accumulate into a single table. Tier 2 of
`CajetaType::ofScoped` / `canonicalNameScoped` then does:

```cpp
auto& imported = importIt->second.begin()->second;   // FIRST entry wins
```

The inner map is `map<string /*packageName*/, …>`, i.e. ordered
lexicographically by package. `cajeta.nucleo.frame` sorts before
`cajeta.xpu`, so once ANY file in the module imports nucleo's
`Capability` (today: `nucleo/frame/ZoneMap.cajeta`), every other file's
`Capability` — including files that explicitly import the xpu one —
resolves to nucleo's.

The behaviour is known and documented at the site, as a v1 simplification:

> First entry wins — multiple imports of the same short name from
> different packages is a future ambiguity-error condition, not a quiet
> pick. v1 just takes one deterministically.

It stopped being theoretical when `cajeta.xpu.Capability` was added.

## Why it is worse than it looks

- **It is silent and non-local.** The failing file is one nobody edited,
  in a package unrelated to either colliding name. Nothing in the error
  names the second `Capability`.
- **It is triggered by an unrelated user import.** `import cajeta.math.Ewise;`
  alone compiles fine; only pulling in a class whose package happens to
  import the other short name breaks it.
- **Adding the "right" import does not help.** The user program importing
  `cajeta.xpu.Capability` explicitly still fails — the pick is made from
  the merged table, not from the referring file.
- **The ordering is alphabetical, so which name wins is arbitrary** and
  will change as packages are added or renamed.

## Fix

Resolve a simple type name against the imports of the compilation unit
that DECLARES the referring code, not against the merged module table.
Concretely, either:

1. Track imports per source file (`imports[file][shortName][package]`) and
   have `ofScoped` consult the current structure's file; or
2. Snapshot each file's import set onto the `CajetaClass`es it declares at
   parse time, and consult that snapshot in tier 2.

(1) is the honest model — imports ARE a per-file construct in the language
— and (2) is the cheaper retrofit.

Whichever lands, a genuinely ambiguous bare name (two imports of the same
short name in ONE file) should be a diagnostic naming both candidates, not
a quiet pick. That is what the existing comment already calls for.

## Not a fix: fully qualifying the use site

`Device.supports(cajeta.xpu.Capability.CoopMatrixBf16F32Acc)` makes the
TYPE resolve correctly, and then fails one layer down:

```
CAJETA_ERROR_ARG_INVALID: argument 1 to `supports` did not lower to a
value — it names an unknown or non-addressable property (:1117)
```

A fully-qualified ENUM CONSTANT does not lower in expression position, so
qualification is not available as a workaround today. (Worth fixing on its
own; a qualified constant is the obvious escape hatch for exactly this.)

## Interim mitigation (taken 2026-08-20)

`cajeta.nucleo.frame.Capability` was renamed to `IndexCapability` — it had
a single live use (`ZoneMap.capability()`), and the new name is more
accurate for a holder of `CHUNK_SKIP` / `RANGE` / `POINT`. That removes
today's collision but NOT the defect: the next two packages to share a
short name will hit it again, silently.

## Acceptance

- Two stdlib packages may both export a class named `X`; a file importing
  one of them resolves `X` to the one IT imported, regardless of package
  name ordering or which other files exist in the module.
- The `DynFrame` repro above builds.
- A single file importing two different `X`es reports an ambiguity
  diagnostic naming both, rather than picking one.
- A fully-qualified enum constant lowers in expression position.
