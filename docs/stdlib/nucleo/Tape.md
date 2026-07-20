# Tape — define-by-run autograd

`cajeta.nucleo.autograd` — the runtime eager tape: ops compute their forward
value as they execute *and* record a node; `backward(out)` replays the
recorded graph in reverse and accumulates gradients. This is the
**dynamic-control-flow** driver — a loop whose trip count arrives at runtime
differentiates here, where the compiled [`Grad`](Transforms.md) (which must
specialize statically) cannot. Both drivers apply the same differentiation
rules and agree wherever both can express a function.

A `Var` is a stack-copied handle to one node; its value and gradient live on
the tape that created it.

```cajeta
package snip.tape;

import cajeta.nucleo.autograd.Tape;
import cajeta.nucleo.autograd.Var;

public final class Demo {
    public static float32 run() {
        Tape t = heap Tape();
        Var x = t.var(3.0f);
        Var y = t.mul(x, x);
        t.backward(y);
        return t.valueOf(y) * 100.0f + t.grad(x);   // 9*100 + 6 = 906
    }
}
```

## The op set

| Op | Records |
|---|---|
| `t.var(v)` | a differentiated input leaf |
| `t.add(a,b)` `t.sub(a,b)` `t.mul(a,b)` `t.div(a,b)` | binary arithmetic |
| `t.neg(a)` `t.exp(a)` `t.log(a)` `t.sqrt(a)` | unary |
| `t.stopGrad(a)` | value passes, gradient does not (the `@NoGrad` analog) |

Readback: `t.valueOf(v)`, `t.grad(v)` (after `backward`), `t.count()`.

## Dynamic control flow — the tape's territory

```cajeta
public static float32 dPow(float32 xv, int32 n) {   // d/dx x^n, n known at RUNTIME
    Tape t = heap Tape();
    Var x = t.var(xv);
    Var y = x;
    int32 i = 1;
    while (i < n) {
        y = t.mul(y, x);
        i = i + 1;
    }
    t.backward(y);
    return t.grad(x);                                // n * x^(n-1)
}
```

The compiled `Grad` rejects this function (non-specializable); the tape
records whichever path actually ran. Use the compiled path for static, hot
functions (it fuses; per-op dispatch never appears), and the tape for
dynamic models, prototyping, and step-by-step gradient debugging.

## Discipline

- **One backward per tape.** A second `backward` throws — there is no silent
  re-accumulation into stale gradients. Record a fresh tape per step.
- Fan-out accumulates: an input feeding several ops receives the sum of its
  cotangents, as it should.
- Tapes are independent — no global state; two tapes never interact.
- `stopGrad` is the disciplined `.detach()`: statically visible in the
  recorded graph, with no version-counter machinery.
