# 06 — Keywords

Every reserved word in Cajeta. The authoritative list is the lexer,
[`antlr4/CajetaLexer.g4`](../../antlr4/CajetaLexer.g4); this table matches it
exactly. A reserved word cannot name a variable, method, or type, except where
a note says it is contextual.

Two familiar words are missing on purpose:

- There is no `new`. Construction names a storage class instead — `heap
  Circle(2.0)` or `stack Circle(2.0)`. See [chapter 10](10-allocation.md).
- `true`, `false`, and `null` are literals, not keywords.

## Primitive type names

| Keywords | |
|---|---|
| `boolean` | truth value |
| `char` | 32-bit Unicode codepoint |
| `int8` `int16` `int32` `int64` `int128` | signed integers |
| `uint8` `uint16` `uint32` `uint64` `uint128` | unsigned integers |
| `float16` `float32` `float64` `float128` | IEEE-754 binary floats |
| `float4e2m1` `float6e2m3` `float6e3m2` `float8e4m3` `float8e5m2` `float8e4m3fnuz` `float8e5m2fnuz` | OCP microscaling floats, storage-only |

[Chapter 08](08-native-types.md) covers all of these. `bfloat16` and `pointer`
are also native types, but they are type *names* resolved like any other, not
reserved words.

## Declarations

| Keywords | |
|---|---|
| `class` `interface` `enum` `record` `annotation` `view` | type kinds ([chapter 09](09-type-kinds.md)) |
| `package` `import` | namespacing |
| `extends` `implements` | inheritance |
| `sealed` `non-sealed` `permits` | sealed hierarchies |
| `void` | no return value |
| `var` | reserved for local type inference; parses but does not resolve yet |
| `mut` | contextual: record field mutation opt-in ([chapter 09](09-type-kinds.md)); an ordinary identifier everywhere else |
| `structure` | reserved; no declaration syntax yet — using it is a parse error |

## Control flow

| Keywords | |
|---|---|
| `if` `else` | branching |
| `switch` `case` `default` `yield` | selection and switch expressions |
| `for` `do` `while` `break` `continue` | loops |
| `return` | method exit |
| `try` `catch` `finally` `throw` `throws` | error handling |
| `assert` | assertions |
| `goto` | reserved, unused — no grammar rule accepts it |

## Objects and expressions

| Keywords | |
|---|---|
| `this` `super` | receiver and parent |
| `instanceof` | runtime type test |
| `operator` | operator-overload declarations |

## Memory and ownership

| Keywords | |
|---|---|
| `heap` | heap construction: `heap Foo(...)` |
| `stack` | stack construction: `stack Foo(...)` |
| `shared` | contextual: workgroup-shared placement inside `@Kernel` bodies only; an ordinary identifier everywhere else |

## Structured concurrency

| Keywords | |
|---|---|
| `async` | method modifier |
| `await` `spawn` `detach` | task expressions |
| `scope` | block that joins all child tasks before exiting |

## Modules (reserved)

`pModule` `open` `requires` `exports` `opens` `to` `uses` `provides` `with`
`transitive` — reserved by the lexer for a future module system. The parser
accepts every one of them as an ordinary identifier today, so methods named
`open` or `requires` keep parsing. Note the placeholder spelling `pModule`.

## Modifiers

| Keywords | |
|---|---|
| `public` `protected` `private` | visibility |
| `static` | type-level member |
| `final` | no override / no reassignment |
| `abstract` | deferred implementation |
| `native` | implemented by the runtime |
| `const` | reserved; the grammar accepts it as a modifier |
| `strictfp` `transient` `volatile` | grammar-level modifiers, Java-familiar |

Next: [Comments](07-comments.md).
