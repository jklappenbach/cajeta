# 13 — Strings & formatting

`String` (in `cajeta.lang`) is a class, not a primitive: immutable UTF-8
text. Every transformation returns a fresh `String`; nothing mutates in
place. Equality is content-based. Like every collection, a `String` reports
its element count with `count()` — codepoints here — while `size()` is the
byte length.

```cajeta
String s = "  Hello, Cajeta!  ";
String trimmed #= s.trim();
boolean has = trimmed.contains("Cajeta");
boolean hello = trimmed.startsWith("Hello");
String sub #= trimmed.substring(7, 13);            // "Cajeta"
String swapped #= trimmed.replace("Cajeta", "World");
String loud #= trimmed.toUpperCase();
int64 codepoints = trimmed.count();
int64 bytes = trimmed.size();
boolean eq = trimmed.equals("Hello, Cajeta!");    // byte-for-byte
```

Run it: [StringDemo](../../samples/tour/src/main/cajeta/tour/lang/StringDemo.cajeta).

## Literals, owned strings, and windows

One type, three storage shapes underneath:

- A **literal** is a view of the program's static data — no allocation,
  never freed.
- A string a method **builds** — `replace()`, `toUpperCase()`, a `+`
  concatenation, `StringBuilder.toString()` — owns a heap buffer. Building
  from your own bytes works too: `heap String(#buf, n)` takes ownership of
  the buffer (the `#` transfers it; the String's drop frees it).
- `substring()` and `trim()` are **zero-copy**: the result is a windowed
  view sharing the source's root buffer — no byte copy, whatever the length.
  Small-string-optimized sources are the exception; a window into an inline
  buffer would dangle, so those materialize a copy.

Transforming methods are declared `#String` (they transfer ownership,
[chapter 11](11-ownership.md)), so bind the result to a plain local and the
drop chain reclaims it at scope exit. Windows follow the slice rules from
chapter 11: a window that escapes its scope keeps the root buffer alive —
the root is promoted to the `shared` state and freed when the last view
drops. You never free a string in any of the three shapes.

`substring(begin, end)` is byte-indexed and half-open; indices clamp to the
valid range. Full method list: [String reference](../stdlib/lang/String.md).

## Concatenation with `+`

`+` concatenates and stringifies its operands — numbers, booleans, and class
values (via `toString()`) all work:

```cajeta
int32 version = 1;
String banner = "Cajeta v" + version + ", pi ~ " + 3.14;
```

Each `+` copies the whole accumulator, so building a long string with
repeated `s = s + chunk` in a loop is O(N²). Accumulate in a `StringBuilder`
instead.

## StringBuilder

A growable byte buffer that materializes once. Short builds (up to 64 bytes)
stay in an inline buffer — a `stack StringBuilder()` build that small does no
heap allocation at all until `toString()`:

```cajeta
StringBuilder sb = stack StringBuilder();
sb.append("Hello, ");
sb.append("world");
int32 soFar = sb.count();          // bytes accumulated
String built #= sb.toString();      // owned #String
```

## Format templates in println

`System.stdout.println(fmt, args...)` treats `{}` in the format string as a
placeholder; each one consumes the next argument in order. Arguments dispatch
through a per-type formatter — integers, floats, `String`, and class values
via `toString()`:

```cajeta
String name = "Cajeta";
int32 version = 1;
float64 pi = 3.14159;
System.stdout.println("hello {}, version {}", name, version);
System.stdout.println("{} + {} = {}, pi ~ {}", 2, 3, 2 + 3, pi);
```

Run it: [FormatStringDemo](../../samples/tour/src/main/cajeta/tour/lang/FormatStringDemo.cajeta).
Full string design: [String spec](../specification/lang/String.md).

Next: [Templates](14-templates.md).
