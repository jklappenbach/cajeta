# `cajeta.lang.Locale` — language / region tag

Companion to `cajeta.lang.String` for the operations whose behaviour
varies by language: case folding, collation, eventually number /
date / currency formatting (those layers are separate; Locale is
the IDENTITY they key off).

Status: **designed, not implemented.** Tracked as task #159 → next
phase becomes a separate impl task. Ships in tandem with the
String implementation so case-folding methods that take a `Locale`
are coherent on day one.

---

## What Locale IS

A small, immutable value type that names a (language, region,
script, variant) tuple. Identity comes from the
[BCP 47](https://tools.ietf.org/html/bcp47) tag — the same
identifier shape that HTTP `Accept-Language`, ICU, browser
`navigator.language`, and modern Java's `Locale.toLanguageTag()`
all speak. Examples:

| Tag       | Language | Region | Script | Variant |
|-----------|----------|--------|--------|---------|
| `en`      | English  | —      | —      | —       |
| `en-US`   | English  | US     | —      | —       |
| `ja-JP`   | Japanese | Japan  | —      | —       |
| `tr-TR`   | Turkish  | Turkey | —      | —       |
| `zh-Hans` | Chinese  | —      | Han Simplified | — |
| `de-CH-1996` | German | Switzerland | — | post-1996 reform |
| `und`     | undetermined | — | — | — — root sentinel |

A Locale value carries those four parts plus the canonical BCP 47
form as a cached String. Construction normalizes the parts
(`en-us` → `en-US`, language lowercase, region uppercase, script
Title-Case) so equal locales compare equal byte-for-byte.

## What Locale is NOT

- **Not a thread-local default.** No `Locale.setDefault()`. Every
  locale-sensitive operation takes the Locale explicitly, or
  defaults to `Locale.ROOT`. The "Turkish-locale-on-the-CI-box
  silently breaks case folding" bug class is engineered out by
  not offering the foot-gun. The cost is a few extra arguments
  at call sites; the benefit is deterministic behaviour.
- **Not a charset.** `Locale.JAPANESE` says nothing about whether
  bytes are UTF-8, Shift-JIS, or EUC-JP. Encoding lives in
  `cajeta.lang.Encoding`.
- **Not a formatter.** `Locale.US` doesn't know how to render
  `1234567.89` as `"1,234,567.89"`. That's a `NumberFormat` /
  `DateFormat` job (separate stdlib doc when those land); Locale
  is the IDENTITY they look up rules by.
- **Not a translation table.** There's no `localize("hello.world",
  Locale.JAPANESE)` resolver in Locale itself. Resource bundles
  are a layer above (planned: `cajeta.lang.ResourceBundle` /
  `cajeta.format.Messages`).

## Surface

```cajeta
public final class Locale {
    // --- Construction ---

    // BCP 47 tag — the canonical entry point.
    // `"en"`, `"en-US"`, `"ja-JP"`, `"zh-Hans-CN"`, `"und"`.
    public static Locale of(String tag);

    // Explicit parts. Any of region/script/variant may be empty;
    // the canonical tag is rebuilt from whatever's supplied.
    public static Locale of(String language,
                            String region,
                            String script,
                            String variant);
    public static Locale of(String language, String region);

    // From the process environment. Reads LC_ALL > LC_CTYPE >
    // LANG and parses the POSIX form (`en_US.UTF-8` → `en-US`).
    // Falls back to ROOT if nothing's set. Pure function — does
    // NOT cache; callers are free to memoize.
    public static Locale fromEnv();

    // --- Predefined constants ---

    public static Locale ROOT;        // "und" — Unicode tables only.
    public static Locale ENGLISH;     // "en"
    public static Locale US;          // "en-US"
    public static Locale UK;          // "en-GB"
    public static Locale CANADA;      // "en-CA"
    public static Locale AUSTRALIA;   // "en-AU"
    public static Locale GERMAN;      // "de"
    public static Locale GERMANY;     // "de-DE"
    public static Locale FRENCH;      // "fr"
    public static Locale FRANCE;      // "fr-FR"
    public static Locale ITALIAN;     // "it"
    public static Locale ITALY;       // "it-IT"
    public static Locale SPANISH;     // "es"
    public static Locale SPAIN;       // "es-ES"
    public static Locale PORTUGUESE;  // "pt"
    public static Locale BRAZIL;      // "pt-BR"
    public static Locale RUSSIAN;     // "ru"
    public static Locale CHINESE;     // "zh"
    public static Locale CHINESE_SIMPLIFIED;   // "zh-Hans"
    public static Locale CHINESE_TRADITIONAL;  // "zh-Hant"
    public static Locale JAPANESE;    // "ja"
    public static Locale JAPAN;       // "ja-JP"
    public static Locale KOREAN;      // "ko"
    public static Locale KOREA;       // "ko-KR"
    public static Locale TURKISH;     // "tr"
    public static Locale ARABIC;      // "ar"

    // --- Accessors ---

    public String language();         // "en", "ja", "zh", ""
    public String region();           // "US", "JP", ""
    public String script();           // "Hans", ""
    public String variant();          // "1996", "" (rare)
    public String toLanguageTag();    // canonical BCP 47, e.g. "en-US"

    // --- Comparison ---

    public boolean operator==(Locale other);
    public int64 hash();              // matches String hashing — same bytes hash equal.
    public int32 compareTo(Locale other);    // lexicographic on toLanguageTag().

    // --- Convenience predicates ---

    public boolean isRoot();          // language().isEmpty() && region().isEmpty() && ...
    public boolean matches(Locale other);   // language match, ignoring region
                                            // "en-US" matches "en", "en-GB" matches "en"
}
```

## Construction examples

```cajeta
Locale tr  = Locale.of("tr-TR");
Locale us  = Locale.US;                  // predefined constant
Locale env = Locale.fromEnv();           // process env

// Equivalent forms:
Locale a = Locale.of("zh-Hans-CN");
Locale b = Locale.of("zh", "CN", "Hans", "");
// → a == b (both normalize to "zh-Hans-CN")
```

## How String uses it

```cajeta
String s = "İstanbul";

// Turkish-correct: capital İ stays İ; dotless i (ı) maps to I.
String tr = s.toLower(Locale.TURKISH);   // "i̇stanbul"

// Locale.ROOT (default): Unicode tables, no language overrides.
String root = s.toLower();               // "i̇stanbul" still works
                                         // because Unicode encodes
                                         // the dotted-I correctly;
                                         // the Turkish path differs
                                         // for the legacy I → ı case.
```

The `Locale.ROOT` default is the foot-gun-free choice: no thread-
local surprise. Code that intentionally wants locale-sensitive
behaviour passes the Locale explicitly.

## Implementation notes

- **Wire format**: the cached `tag` String is the source of truth.
  Constructors normalize and re-stringify; the part accessors
  re-parse the cached tag on demand (cheap — typical tags are
  4–10 chars). An alternative is to store all four parts in the
  struct and never re-parse; both are acceptable.
- **Constants are mode-1 views** over static `.rodata` bytes —
  same shape as String literals. No allocation at startup.
- **`fromEnv()` is pure** — every call re-reads getenv. Callers
  that care about cost should cache the result themselves.
- **Equality is byte-for-byte on canonical tag.** Two Locale
  values with the same normalized tag are `==`. This pins the
  hash, too — `Locale.US == Locale.of("EN-us")` is true; both
  normalize to `"en-US"`.
- **No mutator surface.** Locale values are immutable; there's
  no `withRegion(String)` builder in v1 (could be added if
  composition shows up enough).

## Implementation tier (v1)

What lands at the same time as String:

1. **Value type + identity** — `Locale.of(tag)`, `Locale.of(parts)`,
   the predefined constants, `toLanguageTag()`, part accessors,
   `==` / `hash()`, `compareTo`.
2. **Hook into String** — `String.toLower(Locale)`,
   `toUpper(Locale)`, `compareToIgnoreCase(Locale)`,
   `equalsIgnoreCase(Locale)`. Locale.ROOT default for the no-arg
   forms.
3. **`Locale.fromEnv()`** — POSIX env parse only on v1; Windows
   `GetUserDefaultLocaleName` is a follow-up.

Deferred to later tiers:

- **Number / date / currency formatters** — `NumberFormat.of(locale)`,
  `DateFormat.of(locale)`, etc. Lives in `cajeta.format.*`.
- **Collation** — locale-sensitive sort rules (German `ä` between
  `a` and `b`, Swedish `ä` after `z`). Backed by CLDR collation
  data; not in v1.
- **Bidi** — `Locale.isRightToLeft()` and Bidi engine hooks for
  Arabic / Hebrew. Surface designed around the Unicode Bidi
  Algorithm (UAX #9); deferred until the rendering layer needs it.
- **Display names** — `locale.getDisplayLanguage(in: Locale)` /
  similar, returning the human-readable name in another locale
  ("English" in `en`, "Englisch" in `de`). Needs CLDR display-
  name data; deferred.
- **Resource bundle lookup** — separate package
  (`cajeta.lang.ResourceBundle` planned).

## Open questions

1. **Should `fromEnv()` cache?** Current spec: no caching, pure
   per-call. The lazy-static alternative is ergonomic but
   tangles with test-time env mutation. Lean toward no caching;
   the cost is one `getenv` + a ~10-byte parse.

2. **Backing-store choice** — cache the parsed parts or re-parse
   on each accessor call? Re-parse is simpler and the tag is
   short. Pin in v1 review.

3. **`Locale.matches`** — should this be Java's
   `LanguageRange.matches` (BCP 47 lookup algorithm with weighted
   ranges and fallback chains), or just the simple "shares
   language" semantics shown above? v1 picks simple; the full
   algorithm lands when an `Accept-Language` style negotiation
   shows up in cajeta.io / a server framework.

## See also

- [`String.md`](String.md) — the methods that consume Locale.
- `cajeta-docs/stdlib/Primitives.md` — char vs codepoint, the
  width that case folding works on.
- `cajeta-docs/stdlib/io/Io.md` — `Encoding` is the OTHER
  classification axis (UTF-8 vs Shift-JIS vs …); Locale ≠
  encoding.
- Future: `cajeta.format.NumberFormat`,
  `cajeta.format.DateFormat`, `cajeta.lang.ResourceBundle`.

---

## Changelog

- 2026-05-20: initial spec. BCP 47 tag identity, predefined
  constants, no thread-local default, hooks into String for the
  case-folding methods. Number / date / collation deferred to
  `cajeta.format.*` package.
