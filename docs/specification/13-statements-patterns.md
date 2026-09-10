# 13 — Statements & Patterns

This chapter defines the statement forms: blocks and local declarations, conditionals and patterns, `switch` in both of its forms, loops with labels, `try`/`catch`/`finally`, and `return`. Statements carry ownership obligations — arming drop entries, bounding borrows — that this chapter names and Ownership §5 governs.

## 13.1 Blocks and Local Declarations

A block is a brace-enclosed statement sequence. Declaring an owning local arms a drop entry that fires at the block's closing brace (Allocation §4). A block-nested declaration may shadow an enclosing name (Names §7.1). A live borrow into a value blocks mutation through that value's path for the borrow's extent, and iteration is a borrow construct: mutating a collection inside its own `for`-each body is a compile-time error (Ownership §5.2).

## 13.2 `if` and Pattern `instanceof`

`if (cond) stmt else stmt` selects on a `boolean`. The condition may be a **type pattern**: `expr instanceof T name` tests the dynamic type and, on success, binds `name` as a `T`-typed borrow of the same instance, in scope where the test is true.

**Example 13.2-1.** A pattern binding.

```cajeta
public class Base { public int32 t() { return 1; } }
public class Kid extends Base { public int32 bonus() { return 5; } }
public final class C {
    public static int32 run() {
        Base b = heap Kid();
        if (b instanceof Kid k) { return k.bonus(); }
        return 0;
    }
}
System.stdout.println(C.run());    // 5
```

## 13.3 `switch`

`switch` has two forms.

**The statement form** uses `case label:` arms. Control falls through from one arm into the next unless `break` (or another transfer) ends it, and `default:` catches everything unmatched.

**Example 13.3-1.** Fallthrough and `break`.

```cajeta
public final class C {
    public static int32 fall(int32 v) {
        int32 acc = 0;
        switch (v) {
            case 1: acc = acc + 1;          // falls through
            case 2: acc = acc + 2; break;
            case 3: acc = acc + 4; break;
            default: acc = 99;
        }
        return acc;
    }
}
System.stdout.println("" + C.fall(1) + " " + C.fall(3));    // 3 4
```

**The expression form** uses `case label -> result` arms, does not fall through, and yields a value:

```cajeta
public final class C {
    public static int32 pick(int32 v) {
        return switch (v) { case 1 -> 100; case 2 -> 200; default -> 0; };
    }
}
System.stdout.println(C.pick(2));    // 200
```

> *Discussion.* Two switch surfaces are in the grammar but not usable as of 0.27.0: a block body with `yield` in an arrow arm crashes the compiler, and guarded patterns in `case` labels are unspecified. Both are bound here when they land.

## 13.4 Loops

The loop forms are `for (init; cond; update) stmt`, `for (T v : source) stmt` over an array or an iterable collection binding each element in turn, `while (cond) stmt`, and `do stmt while (cond);`.

A loop statement may carry a label, and `break label;` / `continue label;` transfer to the labeled loop from any nesting depth inside it:

```cajeta
outer: for (int32 i = 0; i < 3; i = i + 1) {
    for (int32 j = 0; j < 3; j = j + 1) {
        if (j == 1) { continue outer; }
    }
}
```

Labeled `break`/`continue` are also part of the kernel device subset (Accelerated Compute §17).

## 13.5 `try`, `catch`, `finally`

`try` guards a block. A `throw` inside it unwinds to the nearest frame whose `catch` clause matches the thrown type (selection is Errors §15.2), dropping every owning local between the throw and the handler on the way (Allocation §4). A `finally` block runs on every exit from the `try` — normal completion, a `return` out of the block, a matched throw, or a throw that passes through unhandled. A `try` may carry a `finally` with no `catch`. Drop order, and a throw raised inside a `finally`, are Errors §15.3.

There is no try-with-resources form: destructors already guarantee deterministic release at the declaring block's closing brace, in LIFO order, on the exceptional path included. Declaring the resource is the pattern.

## 13.6 `return`

`return expr;` ends the method with a value, and `return;` ends a `void` method. A plain `return x` hands back whatever title `x` holds, and `return #x` surrenders it (Ownership §5.5.2). In a script unit, a top-level `return <int32>` is the process exit code (Script Units §18).
