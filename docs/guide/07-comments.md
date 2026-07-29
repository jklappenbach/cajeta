# 07 — Comments

Cajeta has two comment forms, plus a documentation flavor of the second.

## Regular comments

`//` runs to end of line. `/* ... */` spans lines. Both are implementation
notes for whoever reads the source; the documentation tool ignores them.

```cajeta
// implementation note — invisible to the doc tool
int32 cached = -1;      /* block comments work too */
```

## Doc comments

A block comment that opens with `/**` documents the declaration that follows
it — a class, interface, enum, view, method, constructor, field, or annotation
type. The leading `*` on continuation lines is optional but conventional.

The body is CommonMark Markdown: paragraphs, **bold**, lists, tables, inline
code, fenced code blocks, and `[Foo]` / `[Foo.method]` cross-reference links
that resolve against the declaration's import scope.

```cajeta
/**
 * Fixed-rate fee schedule.
 *
 * Rates are in **basis points** (1/100 of a percent). The schedule
 * is immutable once constructed.
 */
public class FeeSchedule {
    /**
     * Fee for a given amount.
     *
     * @Param amount   transaction amount in cents
     * @Return         fee in cents, rounded down
     * @Since 1.0
     */
    public int64 feeFor(int64 amount) {
        return amount * 25 / 10_000;
    }
}
```

## The `@`-tag system

Tags follow the JavaDoc tradition, spelled in CamelCase. Body first, then all
tags in a trailing block. Shipped today: `@Param`, `@Return`, `@Throws` (or
`@Exception`), `@See`, `@Since`, `@Author`, `@Version`, `@SerialField`. Tag
bodies are Markdown too.

A richer set of Cajeta-specific structured tags — `@Owns` / `@Moves` /
`@Borrows` for ownership, `@FiberSafe` / `@Blocks` for concurrency,
`@Complexity`, `@EntryPoint` — is specified but not all wired up yet. The
full tag reference, shipped and planned, is in
[Documentation.md](../specification/buildtool/Documentation.md).

## Generating documentation

`cajeta doc <source-root>` parses the `/** */` comments, renders the Markdown,
resolves cross-references, and writes a static HTML site (default
`build/docs/`). The generator ships behind the `-DCAJETA_BUILD_CAJETADOC=ON`
build option. A package documents itself with a `package.cajeta` file holding
one doc comment above the `package` line.

Next: [Native types](08-native-types.md).
