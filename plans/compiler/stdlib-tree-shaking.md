# Tier 2 — Compiler-side reachability (stdlib tree-shaking)

Status: **scoped, not started.** Tier 1 (prefer LLD for `--emit=exe`, cast-fold for
`(T) literal` static initializers) is landed on `feature/windows-dylib-slimdown`.

## Goal

Build a fat stdlib, emit code only for what the program actually uses, on **all
platforms**. Concretely: a non-TLS program must emit **no** `cajeta.net.tls.*`
method bodies, so there are zero references to the `__cajeta_tls_*` natives, so the
linker never pulls `cajeta_tls.o` / OpenSSL (and likewise Winsock for non-net
programs). This is the only approach that fully works regardless of linker quirks —
see "Why not a linker-only fix" below.

## Why the linker alone can't finish the job (Tier 1 findings)

- `--gc-sections`/`-dead_strip` already strip the vast majority of the stdlib
  (~2526 of ~2573 fn sections in a HelloWorld). The design is sound.
- Net/TLS is **IR-unreachable from every root** (verified by full call-graph BFS
  incl. data globals → 0 net-ish nodes) yet survives linking:
  1. GNU `ld` COFF `--gc-sections` keeps unreferenced external COMDAT sections.
     **LLD fixes this** (Tier 1: 9.2 MB → 6.7 MB). But LLD still keeps a residual
     `TlsConnection::server`/`connNew` island.
  2. `cajeta_tls.o` (mingw-gcc) has **non-COMDAT** `.text$__cajeta_tls_*` sections →
     linkers treat them as non-collectable roots → all of OpenSSL rides in. And the
     always-linked `stdlib.o` thunks reference `__cajeta_tls_*`, so the linker
     **pulls `cajeta_tls.o` during symbol resolution, before GC runs** — even as a
     `.a` member. `lld-link` has no `--start-lib` (ELF-only) to defer the pull.
- Net: as long as `stdlib.o` *contains* the TLS thunks and they reference the
  natives, the natives (and OpenSSL) come along. The fix is to **not emit the
  unused thunks** → reachability.

## Algorithm — Rapid Type Analysis (RTA) worklist

Replace the eager Phase-1/2 loop (`Compiler.cpp` ~774-795, currently
`for module: for method: generateCode()`) with a reachability-driven worklist.

Architecture notes (confirmed):
- `Method::getLlvmFunctionType()` lazily builds the prototype (`generatePrototype`);
  `Method::generateCode()` emits the body. Today both run for every method.
- Vtables reference their virtual-method impls; RTTI references the vtable; **static
  methods (e.g. `connNew`) are only reachable via a direct call** — exactly what we
  want to drop.

Worklist:
1. **Roots**
   - Entry method (`entryMethod`).
   - C main shim callees: `__cajeta_property_install`, `__cajeta_args_make`,
     `strncmp`.
   - Surviving global ctors: `__cajeta_runtime_init`, `__cajeta_hash_seed_init`,
     `__cajeta_register_unrecoverable_vtable`, plus any reachable class clinit and
     any `@Kernel` registration ctor.
   - ABI/runtime-required: UnrecoverableException machinery, exception personality,
     drop-chain entry points the ABI calls.
2. **Process a reachable method** (`generateCode`, then scan for):
   - Direct (static/non-virtual) call → add callee.
   - Static field / static method access → add owning class's clinit + static
     globals.
   - `new` / instantiation → mark class **instantiated**; add its constructor; emit
     its vtable (which forces its virtual-method impls — drop/toString/hash/clone +
     overrides).
   - **Virtual call** `Base::m` → add `Base::m` and, for every **instantiated** class
     `C` that overrides `m`, add `C::m`. Maintain the instantiated-set; when a new
     class becomes instantiated, revisit recorded virtual call sites; when a new
     virtual call site appears, add overrides of already-instantiated classes.
   - Interface dispatch → same over implementing classes.
3. **Fixpoint** until the worklist drains (subsumes the existing template-
   instantiation-discovery re-iteration).
4. **Emit** only worklist-reached methods. Unreached methods emit nothing.

## Correctness risks (ranked)

1. **RTA soundness for virtual/interface dispatch.** Missing an override of a called
   virtual method in an instantiated class = call into a stripped function = crash.
   This is the core risk and the bulk of the test burden.
2. **Reflection.** If cajeta supports reflective construct/invoke (RTTI carries
   method metadata) or reflective deserialization, those targets are invisible to
   static reachability. Mitigation: inventory the reflection surface; conservatively
   keep all methods of any class reached *and* reflection-eligible, or add a
   `@Keep`/keep-list and keep reflectively-named classes whole. **Must inventory
   before flipping the default.**
3. **AOP (`@Around`/advice).** Matched advice + `__original` must be roots for any
   matched, reachable method.
4. **DI (`@Component`/`@Inject`).** Synthesized factories/singletons must root the
   component methods they instantiate.
5. **vtable slot completeness** — handled by rule 2 (emitting a vtable forces its
   slots), but must be airtight for inherited slots.

## Emit-mode policy

- `--emit=exe` / `--emit=uber`: whole-program → tree-shake from the entry.
- `--emit=cja`: **library** — no single entry; keep everything (public API is the
  surface). Do **not** tree-shake.
- `--emit=obj`: linkable into anything → keep all, or require an entry to opt in.

## Phasing

- **Phase A (analysis only, low risk):** instrument `generateCode` to record the
  reference graph (callees / instantiations / field refs) without changing emission.
  Compute the reachable set and **diff it against the full set** — report what would
  be stripped for representative programs (HelloWorld, a net server, a GPU sample).
  Validates the model with zero behavior change.
- **Phase B (gated emission):** gate emission on reachability for `exe`/`uber` behind
  `--tree-shake=on|off` (default **off**). Verify net/TLS strips and HelloWorld drops
  OpenSSL/Winsock on all three platforms. Run the full suite.
- **Phase C (soundness + default-on):** RTA for virtual/interface; reflection
  keep-list; AOP/DI roots. Flip default on once green.

## Effort

Substantial / multi-day. Phase A moderate; Phase C (virtual-dispatch soundness +
reflection) is the hard, risky part and where most test effort goes.

## Once Tier 2 lands

`cajeta_tls.o` and `-lssl/-lcrypto` can become **demand-only** (archive members):
with no TLS thunks emitted there are no `__cajeta_tls_*` references, so they're never
pulled. A TLS-using program emits the thunks → references the natives → pulls them.
True pay-for-use, no per-mode special-casing.
