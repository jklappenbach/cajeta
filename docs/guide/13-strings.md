# 13 — Strings & formatting

`String` (in `cajeta.lang`) is a class, not a primitive: immutable UTF-8
text. Every transformation returns a fresh `String`; nothing mutates in
place. Equality is content-based. Like every collection, a `String` reports
its element count with `count()` — codepoints here — while `size()` is the
byte length.

```cajeta
String s = "  Hello, Cajeta!  ";
String trimmed = s.trim();
boolean has = trimmed.contains("Cajeta");
boolean hello = trimmed.startsWith("Hello");
String sub = trimmed.substring(7, 13);            // "Cajeta"
String swapped = trimmed.replace("Cajeta", "World");
String loud = trimmed.toUpperCase();
int64 codepoints = trimmed.count();
int64 bytes = trimmed.size();
boolean eq = trimmed.equals("Hello, Cajeta!");    // byte-for-byte
```

Run it: [StringDemo](../../samples/tour/src/main/cajeta/tour/lang/StringDemo.cajeta).

## Literals vs owned strings

A string literal is a **view**: it points at the program's static data, costs
no allocation, and is never freed. A string a method *builds* — the result of
`trim()`, `substring()`, `replace()`, a `+` concatenation — is an **owned**
heap value. Transforming methods are declared `#String` (they transfer
ownership, chapter 11), so bind the result to a plain local and the drop
chain reclaims it at scope exit. You never free a string either way; the
distinction only matters as "literals are free, built strings are owned by
whoever binds them".

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
String built = sb.toString();      // owned #String
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
