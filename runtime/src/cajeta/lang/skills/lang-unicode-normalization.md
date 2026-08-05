# cajeta.lang — Unicode normalization, case folding, and the UCD

New in the stdlib-completion batch (spec `stdlib-completion` §7). The
FULL Unicode 16.0.0 Character Database ships compiled into the runtime
(~460 KB of generated tables, `tools/ucd/gen_ucd_tables.py`), so results
are identical on every platform — no ICU, no host variance. Conformance:
the complete `NormalizationTest.txt` passes (test/ucd/).

## Normalization — `String.nfc() / nfd() / nfkc() / nfkd()`

Composed and decomposed spellings of the same text differ byte-wise;
normalize both before comparing (§7.2):

```cajeta
String a = "café";           // composed é
String b = "café";     // e + combining acute (conceptually)
boolean same = a.nfc().equals(b.nfc());   // true
```

`isNfc()` / `isNfd()` answer EXACTLY (quick-check fast path, transform
fallback). Already-normalized input returns a zero-copy borrow window —
no byte copy. Malformed UTF-8 throws `EncodingException` rather than
producing garbage.

## Case folding — `String.caseFold()`

FULL Unicode case folding, for caseless MATCHING — deliberately distinct
from `toLowerCase` (ASCII-only): ß folds to "ss", İ to "i" + combining
dot. Fold both sides, then compare:

```cajeta
boolean match = a.caseFold().equals(b.caseFold());
```

## Stripping — `String.stripDefaultIgnorables()`

Removes soft hyphens, zero-width joiners, and the other
Default_Ignorable_Code_Point characters. Separable from normalization —
neither implies the other.

## Properties — `cajeta.lang.Ucd`

Per-codepoint script id, joining type (`Ucd.JOIN_*`), canonical
combining class, bidi class, default-ignorable — the seam
`cajeta-text-shaping` §13.2 consumes. Same tables as normalization; a
second copy of the UCD anywhere would be indefensible.
