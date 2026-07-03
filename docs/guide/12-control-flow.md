# 12 — Control flow

The grammar is C-family. Conditions are parenthesized; bodies use braces.
Run the examples:
[ControlFlowDemo](../../samples/tour/src/main/cajeta/tour/lang/ControlFlowDemo.cajeta)
and
[SwitchTernaryDemo](../../samples/tour/src/main/cajeta/tour/lang/SwitchTernaryDemo.cajeta).

## if / else, while, do

```cajeta
int32 evenSum = 0;
int32 oddSum  = 0;
int32 i = 1;
while (i <= 5) {
    if (i % 2 == 0) {
        evenSum = evenSum + i;
    } else {
        oddSum = oddSum + i;
    }
    i = i + 1;
}

int32 n = 0;
do {
    n = n + 1;        // body runs at least once
} while (n < 3);
```

## for and enhanced-for

```cajeta
int32 fact = 1;
for (int32 k = 1; k <= 5; k = k + 1) {
    fact = fact * k;
}

int32[] xs = {1, 2, 3, 4};
int32 sqSum = 0;
for (int32 x : xs) {          // x is a per-iteration borrow
    sqSum = sqSum + x * x;
}
```

## break and continue

Both work unlabeled and labeled; a labeled `break` exits the named loop.

```cajeta
int32 sum = 0;
for (int32 i = 0; i < 10; i = i + 1) {
    if (i == 5) { break; }
    if (i % 2 == 0) { continue; }
    sum = sum + i;
}

outer: for (int32 a = 0; a < 3; a = a + 1) {
    for (int32 b = 0; b < 3; b = b + 1) {
        if (a + b == 3) { break outer; }
    }
}
```

## switch — statement and expression

The classic statement form falls through unless you `break`; stacked labels
(`case 2: case 3:`) share one arm.

```cajeta
int32 day = 3;
int32 out = 0;
switch (day) {
    case 1:
        out = 10;
        break;
    case 2: case 3:       // multi-label fall-through
        out = 23;
        break;
    default:
        out = 99;
        break;
}
```

The arrow form is an expression: each arm is a single expression, there is no
fall-through, and the whole switch can be assigned, returned, or embedded.

```cajeta
int32 code = 2;
int32 r = switch (code) {
    case 1 -> 100;
    case 2, 3 -> 200;     // multi-label arm
    default -> -1;
};
int32 sum = 1000 + switch (code) {
    case 2 -> 7;
    default -> 0;
};
```

## Ternary and instanceof

`cond ? a : b` evaluates only the selected branch; mixed-type branches widen
to the larger type (`int32` vs `float64` below).

```cajeta
int32 a = 5;
int32 b = 10;
int32 min = a < b ? a : b;
float64 mixed = a < b ? 1.5 : 2;      // widens to float64

boolean isInt = a instanceof int32;
String tag = mixed instanceof float64 ? "f64" : "other";
```

`instanceof` currently matches against the operand's *static* type; it does
not yet walk the runtime class hierarchy for class types.

Next: [Strings & formatting](13-strings.md).
