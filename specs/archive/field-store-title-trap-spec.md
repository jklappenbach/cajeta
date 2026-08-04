# Field-store title trap — NOT a defect; the ownership contract of `heap` at a call site

Origin: compiler-mcp Unit 5 skill-example verification, 2026-07-31.
**Re-diagnosed twice.** The original filing
(`template-field-borrow-escape-spec.md`) was wrong on both counts. This
document's own §4 then proposed a semantic change that was ALSO wrong.
**Closed 2026-08-02:** the compiler is correct, the proposal is withdrawn, and
the remaining work was documentation. See §4 and §5.

## 1. The shape

```cajeta
public class BoxA {
    public Animal value;
    public BoxA(Animal v) { this.value = v; }   // borrow
}
BoxA b = heap BoxA(heap Animal(2));             // `heap` at a call site SURRENDERS
return b.value.tag;                             // reads freed memory
```

## 2. Why this is correct behavior

Two rules compose, and both are working:

1. **`heap X(...)` at a call site surrenders.** That is the implicit
   transfer/ownership contract of `heap`: the formal `v` becomes the runtime
   OWNER of the new object.
2. **`=` is a borrow and never inherits that contract.** A plain store aliases;
   it does not consume the formal's title.

So the formal still owns the object when the constructor returns, its drop
fires, and the field is left pointing at freed memory. The program asked to
borrow from a value whose owner dies at the end of the call. The compiler did
what was written.

`#=` is the spelling that transfers, and the formal stays plain:

```cajeta
public BoxA(Animal v) { this.value #= v; }      // field takes the title
```

## 3. Verified behavior (2026-08-02)

| Store | Caller LENDS (named local) | Caller SURRENDERS (`heap` rvalue) |
|---|---|---|
| `this.value = v` | correct — field aliases, local still owns | **dangles** — author error (§2) |
| `this.value #= v` | correct — safe, both readable | correct — field owns |

Two corrections to earlier drafts, both measured:

- **A `#T` formal is NOT required.** A PLAIN formal with a `#=` store transfers
  correctly. Earlier drafts — and the shipped `language-ownership` skill —
  prescribed "declare `#T` and store with `#=`", which overstates the fix.
  `#T` is API-visible and forces every caller to surrender; changing the store
  alone is enough, and is not a breaking change.
- **`#=` is safe against either caller shape.** It does not blindly claim a
  title from a lending caller. That is what makes "use `#=` when you mean
  transfer" a rule an author can follow at the store site without reasoning
  about how every caller passes the value.

## 4. Withdrawn proposal

An earlier §4 proposed making a plain `=` field store consume the formal's
runtime title when the caller had surrendered.

**Withdrawn 2026-08-02.** It would make `=` *sometimes* a transfer, decided by
the caller rather than by the store site — destroying the property that gives
`#=` its meaning. The value of the current design is that the store site alone
decides and an author can read one line and know what it does. Rescuing a
mis-written program is not worth making every correctly-written one ambiguous.

Also rejected, and still rejected: a compile-time diagnostic. At the STORE site
it cannot distinguish the corrupting call from the legitimate one — the
discriminator is the *caller's* runtime flag — so it would fire on every
legitimate aliasing store in the stdlib (`ArrayStream<T>.data` aliases
`ArrayList<T>.data`; `Optional<T>.value` aliases its ctor argument). At the
CALL site, a rule like "`heap` rvalue into a plain formal" over-fires:
`foo(heap Animal(2))` for a non-escaping read is legitimate and common. The
general form is escape analysis, which `FieldOwnership.md` deliberately
removed.

## 5. Disposition — documentation, and it was wrong

No compiler change. The hazard WAS already carried in the shipped
`runtime/skills/language/language-ownership.md` (served to agents via
`cajeta search-skill` and the compiler MCP) — but it prescribed the `#T`
signature change rather than the one-token store fix. Corrected 2026-08-02.

Where authors meet this:

- `language-ownership` (compiler-embedded) — now explains the two composing
  rules, gives `#=` on a plain formal as the fix, and notes `#T` as a stronger,
  API-visible choice rather than the remedy.
- `dev.cajeta.ml`'s `ml-training` skill lists it as a hazard. It bit
  `SpelaTrainer` and `BackpropTrainer` during the cajeta-ml v3 arc — twice, in
  carefully written code, which is why it earned a spec at all.
- The `language-templates` skill was corrected when the template-specific claim
  was retracted.

## 6. Tests

`test/expression/SignatureAbiTests.cpp`:

- `plainFormalSharpStoreTakesTitleFromSurrenderedRvalue` — the fix, with a
  plain formal.
- `plainFormalSharpStoreIsSafeWhenCallerLends` — `#=` does not double-claim.
- `lentLocalIntoPlainFormalFieldStoreAliases` — the ratified aliasing path.
- `owningTemplateFieldStoreCompilesAndRuns` — the `#T` + `#=` shape.

`DISABLED_freshRvalueIntoPlainFormalFieldStoreDangles` was REMOVED. It asserted
the trapped program should return 2 — that the compiler should rescue it —
which §2 shows is the wrong expectation. Leaving it disabled implied a fix was
owed.
