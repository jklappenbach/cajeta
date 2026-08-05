# owned-return-of-borrowed-this — defect

Found 2026-08-05 implementing `stdlib-completion` U7 (String normalization
fast paths).

## 1. Defect

`return this;` from a `#T`-returning instance method COMPILES, and the
returned value behaves as a fresh OWNED reference to the receiver's
object. The caller's temp then legitimately drops it — freeing the
wrapper out from under the receiver local, which dangles. Use-after-free
on the next touch; in practice it detonates one JIT session later, when
the freed block gets reused (observed as a SIGSEGV in jitted String code
on the second session of a test process).

Repro shape (was `String.normalizeForm`'s fast path):

```cajeta
public #String nfc() {
    ...
    if (alreadyNormalized) {
        return this;          // receiver is a BORROW; '#' return makes
    }                          // the caller OWN it — double title
    return #fresh;
}
// caller:
String d = s.nfd();            // d owns a native-created String
d.nfd();                       // fast path returns this -> temp owns d's
                               // wrapper -> temp drop frees it
d.nfc();                       // UAF
```

## 2. Where it lived

- `String.replace` shipped this shape for a long time ("no match returns
  this") — latent because callers rarely chain off a no-change result of
  an OWNED receiver before its scope ends, and literals (static bit) are
  drop-immune.
- The U7 normalization fast paths copied the idiom and hit it
  immediately (`d.nfd()` on a just-created owned string).

## 3. Workaround (live, 2026-08-05)

Return a ZERO-COPY BORROW WINDOW instead of the receiver —
`Cajeta.stringSliceBorrow(this, 0, this.byteLength())` (trim's recipe):
no byte copy, a fresh 32-byte wrapper with the borrow bit, safe to own
and drop. Applied to `String.replace`, `nfc/nfd/nfkc/nfkd`, `caseFold`,
`stripDefaultIgnorables`.

## 4. The compiler question

The borrow checker should either REJECT `return this` from a
`#`-returning method (the receiver is a plain-borrow formal — the method
has no title to transfer; this matches the field-store-title-trap
doctrine that `=`/plain formals never inherit ownership), or lower it to
an implicit share/retain where the runtime supports one. Rejection with
a diagnostic naming the borrow-window idiom is likely the right v1.

## 5. Acceptance

- **5.1** `return this` from a `#T` method is either rejected at compile
  time with a diagnostic, or compiles to something that does NOT free the
  receiver's wrapper when the returned temp drops.
- **5.2** The §1 repro chain (`d.nfd(); d.nfc();` across two JIT sessions
  in one process) stays green.
- **5.3** The borrow-window workarounds in `String` can stay (they are
  also faster than a retain), but the language no longer depends on
  library discipline for soundness here.
