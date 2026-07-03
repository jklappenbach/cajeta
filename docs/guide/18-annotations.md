# 18 — Annotations & synthesis

An annotation is `@Name` before a declaration, optionally with arguments:
`@Order(2)`, `@Component(name = "primary")`, `@Labels({"hot", "live"})`.
Any identifier is a valid annotation. One the compiler doesn't recognize is
inert — no code generated, no overhead — until an aspect pointcut
([chapter 19](19-di-aspects.md)) or reflection ([chapter 21](21-reflection.md))
reads it back.

Tour demo: [AnnotationsDemo](../../samples/tour/src/main/cajeta/tour/lang/AnnotationsDemo.cajeta).

## The synthesis family

A Lombok-style family of annotations makes the compiler write the boilerplate.
Each synthesized method is an ordinary method: visible to reflection,
debuggable, and skipped if you declare the same signature by hand — your
version wins.

| Annotation | Synthesizes |
|---|---|
| `@Getter` / `@Setter` | Accessors, `size()`-style naming: `name()`, `name(v)` |
| `@ToString` | `toString()` as `ClassName(field=value, ...)` |
| `@AutoHash` | The hash method |
| `@NoArgsConstructor` / `@AllArgsConstructor` / `@RequiredArgsConstructor` | Constructors |
| `@Builder` | A fluent builder + static `builder()` |
| `@With` | Per-field copy-with methods (`withX(...)`) |
| `@NonNull` | Null checks at method entry / construction / return |
| `@Data` | Bundle: getters, setters, `toString`, hash, required-args ctor |
| `@Value` | Immutable bundle: getters, `toString`, hash, all-args ctor — no setters |

`@Cleanup` (Lombok's scoped close) is planned but not implemented; the drop
chain already releases owned locals at scope exit ([chapter 20](20-error-handling.md)).

## @Builder

```cajeta
@Builder
@ToString
public class Book {
    public String title;
    @Builder.Default public int32 pages = 100;
}
```

`@Builder.Default` makes the field initializer the fallback when the caller
skips that setter:

```cajeta
Book b = Book.builder()
    .title("The Mythical Man-Month")
    .pages(322)
    .build();
Book d = Book.builder().title("Untitled").build();
System.stdout.println("default pages = " + d.pages);
```

## @Value

```cajeta
@Value
public class Rgb {
    int32 r;
    int32 g;
    int32 b;
}
```

`@Value` gives an all-args constructor and getters named after the fields:

```cajeta
Rgb c = stack Rgb(230, 126, 34);
System.stdout.println("r = " + c.r());
System.stdout.println(c.toString());
```

## Declaring your own

The `annotation` keyword declares a new annotation type:

```cajeta
annotation Traced { }
```

Apply it like any other:

```cajeta
public class PaymentService {
    @Traced public void charge(int32 cents) {
        return;
    }
}
```

`@Traced` here does nothing by itself. It becomes load-bearing as an aspect
pointcut (an `@Before(Traced.class)` advice fires on every `@Traced` method —
next chapter) or through reflection (`method.hasAnnotation(...)`,
[chapter 21](21-reflection.md)). Annotations can carry arguments — scalars,
strings, class literals, lists — and reflection reads the values back.

Full surface: [the annotations specification](../specification/reflect/Annotations.md).

Next: [DI & aspects](19-di-aspects.md).
