# ToDo

Working tracker for the next chunks of compiler work. Replaces the per-rollout status files now that the struct/view + memory-model rollouts are both complete (archived in `cajeta-docs/history/`).

Convention: each entry is a brief description, why it matters, where it bites today, and any pointer at where the design discussion lives. Mark `[x]` when complete; promote items into a session/rollout doc if the work warrants one.

---

## Current state (2026-05-17, late session)

Tree at 791/791. Recently landed:

- ✅ **Phase 7 collapse** (`4dfbfd1`) — `struct` is a transitional alias for `class`; CajetaAggregate retired; CajetaView rebased on CajetaClass; 9 struct-specific test files deleted; ~105 now-irrelevant tests retired.
- ✅ **P6.5 grammar** (`1beedc7`) — `(T) -> void` parses as a method-parameter type.
- ✅ **P6.5 lambda-arg expectedType propagation** (`87fa4a2`) — lambda literals as direct method args inherit expectedType from the target method's signature when the lookup is unambiguous (same name + arg count).
- ✅ **P6.5 Stream.forEach** (`40d1e55`) — first lambda-taking terminal lands in `cajeta.lang.Stream`. No-capture lambdas walk correctly through the virtual-dispatch path on ArrayStream<int32>.
- ✅ **Q11 Optional.get throw** (`3f0d99a`) — Optional.get() throws CAJETA_ERROR_NONE_UNWRAP (encoded int 1) on empty. Standard try/catch shape catches it.
- ✅ **P6.7 ArrayList** (`f44dcb6` + `bcc9f05`) — `cajeta.collection.ArrayList<T>` with ctor / size / isEmpty / get / set / add / stream(). T[] backing array, doubles capacity on grow.

## Remaining work (in priority order)

1. **Lambda capture of static fields — pre-existing limitation, 1 session.** Lambdas that read or write static fields (`S.total`) crash at runtime. Surfaces when calling Stream.forEach with a capture lambda (works for no-capture lambdas; see StreamTests). The L2 closure ABI carries captures, but static-field references aren't real "captures" in the locals sense — they're module-level globals reached through static resolution. Fix: either (a) treat static-field refs as non-captures and resolve at the lambda body's emission site, or (b) add them to the captures struct correctly. Probe to reproduce: any lambda body that does `S.someStaticField = ...` then call lambda directly.

2. **P6.7+ — more collections — multi-session.** HashSet (HashMap-backed thin wrapper), HashMap.entries() / keys() / values() returning Streams (the missing axis-wise iteration), LinkedList, Collector<T,R> + cajeta.lang.Collectors. Each is its own piece.

3. **Stream lambda combinators — multi-session.** map, filter, flatMap, take, skip, peek, fold, reduce, anyMatch, allMatch, noneMatch, findFirst, collect. Each is its own concrete *Stream wrapper class plus the corresponding method on Stream<T>. The forEach pattern (`(T)->void` lambda) generalizes to most of these; those returning a new stream need to construct wrapper instances.

4. **Generic-static-factory call syntax — needs method-level generics first.** `Optional<int32>.Some(42)` doesn't parse. The grammar rejects `public static <T> Box of(T arg)` — the typeParameters slot exists for interface methods (interfaceCommonBodyDeclaration) but not for concrete methodDeclaration. Add `typeParameters?` to methodDeclaration, then wire visitor + dispatch.

5. **P6.6 chained-form completion — 1 session.** `xs.stream().count()` direct chain. Setting resolvedType on the inner stream MCE in generateCode breaks ~100 unrelated tests; the cleaner path is to either thread the user module into TemplateInstantiator's structures map (so cross-module merge picks up the methods) or override resolveTypes to do the lookup without instantiation.

6. **P5 — live-borrow tracker — 1 session.** Extends path-borrow machinery to track live read-borrows for iterator invalidation. "Comparable to one of the S6-S11 sessions" per Q10.

7. **P3c — switch/loops/try-catch DA merging — 0.5 sessions each, deferred until consumed.** Implementation pattern is clear from P3a/P3b.

8. **Phase 7 cleanup — 0.5 sessions, not urgent.** Strip the 15 dead `dynamic_pointer_cast<CajetaStruct>` expressions and delete CajetaStruct.h. Cosmetic.

9. **Restore lost test coverage from Phase 7 — incremental, as Phase 6 progresses.** The 9 deleted struct test files contained ~105 tests. Many exercised happy-path behavior still valid under the unified model.

---

## Done

(See "Current state" above for the running list; older entries below.)
