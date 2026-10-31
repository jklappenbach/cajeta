# 13 — Strings & formatting

`String` (in `cajeta.lang`) is a class. It holds immutable UTF-8 text.
Every transformation returns a fresh `String`, and nothing mutates in
place. Equality is content-based. Like every collection, a `String` reports
its element count with `count()`, which counts codepoints here, while
`size()` is the byte length.

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

One type has three storage shapes underneath it:

- A **literal** is a view of the program's static data. It allocates
  nothing and is never freed.
- A string that a method builds, such as `replace()`, `toUpperCase()`, a `+`
  concatenation or `StringBuilder.toString()`, owns a heap buffer. Building
  from your own bytes works too. `heap String(#buf, n)` takes ownership of
  the buffer, the `#` transfers it, and the String's drop frees it.
- `substring()` and `trim()` are zero-copy. The result is a windowed view
  that shares the source's root buffer, and no byte is copied whatever the
  length. Small-string-optimized sources are the exception. A window into an
  inline buffer would dangle, so those materialize a copy.

Transforming methods are declared `#String`, which means they transfer
ownership ([chapter 11](11-ownership.md)). Receive the result with `#=`, as
in `String trimmed #= s.trim();`, and the drop chain reclaims it at scope
exit. The `#` goes on the store and never on the local's type. A plain `=`
on a `#String` result is `CAJETA_ERROR_OWNED_RESULT_NEEDS_TRANSFER`. Windows
follow the slice rules from chapter 11. A window that escapes its scope keeps
the root buffer alive, since the root is promoted to the `shared` state and
freed when the last view drops. You never free a string in any of the three
shapes.

`substring(begin, end)` is byte-indexed and half-open, and indices clamp to
the valid range. The full method list is in the
[String reference](../stdlib/lang/String.md).

## Concatenation with `+`

`+` concatenates and stringifies its operands. Numbers, booleans, and class
values (via `toString()`) all work:

```cajeta
int32 version = 1;
String banner = "Cajeta v" + version + ", pi ~ " + 3.14;
```

Each `+` copies the whole accumulator, so building a long string with
repeated `s = s + chunk` in a loop is O(N²). Accumulate in a `StringBuilder`
instead.

## StringBuilder

A `StringBuilder` is a growable byte buffer that materializes once. Short
builds (up to 64 bytes) stay in an inline buffer, so a `stack StringBuilder()`
build that small does no heap allocation at all until `toString()`:

```cajeta
StringBuilder sb = stack StringBuilder();
sb.append("Hello, ");
sb.append("world");
int32 soFar = sb.count();          // bytes accumulated
String built #= sb.toString();      // owned #String
```

## Format templates in println

`System.stdout.println(fmt, args...)` treats `{}` in the format string as a
placeholder, and each one consumes the next argument in order. Arguments
dispatch through a per-type formatter, which covers integers, floats,
`String`, and class values via `toString()`:

```cajeta
String name = "Cajeta";
int32 version = 1;
float64 pi = 3.14159;
System.stdout.println("hello {}, version {}", name, version);
System.stdout.println("{} + {} = {}, pi ~ {}", 2, 3, 2 + 3, pi);
```

Run it: [FormatStringDemo](../../samples/tour/src/main/cajeta/tour/lang/FormatStringDemo.cajeta).
The full string design is in the [String spec](../specification/lang/String.md).

Next: [Templates](14-templates.md).
