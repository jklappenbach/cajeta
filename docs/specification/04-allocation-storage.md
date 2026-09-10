# 4 — Allocation & Storage

This chapter defines Cajeta's allocation model: the `stack` and `heap` allocation expressions, the per-thread drop chain that reclaims owned values, and the storage behavior of stack instances. Reclamation is automatic and deterministic. There is no garbage collector, no `delete`, and no `free`.

## 4.1 Allocation Expressions

Every class and array allocation is written with `heap` or `stack` at the allocation site:

```cajeta
MyClass a = heap MyClass();      // heap: lives until its owner releases it
MyClass b = stack MyClass();     // stack: lives in the current frame
int8[] buf = heap int8[4096];
```

`heap` directly replaces `new`: it allocates on the heap and reads the same way at the use site. Where other languages leave stack allocation to compiler analysis or a separate mode, Cajeta introduces `stack` as an explicit keyword for stack-based objects. Calling the choice out at every allocation aids readability: a reader — and especially a beginning developer — sees where each instance lives, and how its code will behave, without consulting anything outside the line.

A `heap` variable implies ownership: the variable that receives the allocation owns the instance, and the developer decides the owning scope by deciding where that variable lives. Ownership of a heap object can be transferred between variables, and a heap object can be borrowed without moving ownership. Ownership §5 defines both. The instance is released when its owner drops (§4.2).

The keyword is not part of the type: `stack MyClass()` and `heap MyClass()` produce the same type `MyClass` (Types §3.2), and a method receiving a `MyClass` does not know or care where it was allocated.

Allocation is always explicit at the use site. There is no implicit boxing, no implicit copy construction, and no implicit heap traffic.

An anonymous `heap T(...)` expression in transfer position — a field store, an argument, a `#T` return — promotes implicitly, with no `#` written: the temporary is an unnamed owner with no prior identity, so promotion has no use-after-move risk (Ownership §5.3).

## 4.2 The Drop Chain

Both owning and `stack` objects are dropped when they reach the end of their scope — a method invocation, or a scope block inside a method (Types §3.3). There is no explicit method call to delete an object: destruction and reclamation are handled entirely as part of the drop.

The mechanism is a per-thread chain of drop entries. Declaring an owning or `stack` local arms an entry, and the entries of a lexical block fire at the block's closing brace, in reverse declaration order. Firing an entry runs the instance's destructors and, for a heap instance, frees its memory (Ownership §5.9 specifies how transfer moves an entry's obligation).

A class may declare a destructor, `~ClassName()`, to release resources the instance holds — close a file, return a connection. It is not user-callable. Only the drop chain invokes it, exactly once per instance, and chaining is automatic — the class's own destructor body runs, then each ancestor's. Classes §8.8 gives the full rules. A class with no destructor still drops — its owned fields are released as part of the drop (Ownership §5.8).

Drops fire on the exceptional path too: a `throw` unwinds the chain to the enclosing try frame's watermark, so every owning local between the throw and the handler is dropped before the handler runs.

**Example 4.2-1.** Reverse order, block scoping, and the exceptional path.

```cajeta
import cajeta.error.Exception;
public class Probe {
    public int32 id;
    public Probe(int32 id) { this.id = id; }
    public ~Probe() { System.stdout.println("drop " + this.id); }
}
public final class C {
    public static void run() {
        Probe a = heap Probe(1);
        Probe b = heap Probe(2);
        { Probe c = heap Probe(3); }          // drops at this inner brace
        try {
            Probe d = heap Probe(4);
            throw heap Exception("boom");     // d drops before the handler runs
        } catch (Exception e) {
            System.stdout.println("caught");
        }
    }                                          // then b, then a — reverse order
}
C.run();
```

The program prints `drop 3`, `drop 4`, `caught`, `drop 2`, `drop 1`.

## 4.3 Stack Allocation

A stack instance lives in the current frame's stack region. Its drop entry runs the destructor at the declaring block's closing brace like any other, and the bytes themselves are reclaimed at frame teardown.

A method may return a stack-allocated value by value: the caller pre-allocates the return body in its own frame and passes the slot to the callee, which constructs into it. The bytes never live in the callee's frame, so nothing escapes and nothing is heap-allocated. This applies to any stack-allocated class returned from any method.

A `#`-return of a stack local is a compile-time error, `CAJETA_ERROR_STACK_RETURN_ESCAPES`: `#` promises the caller an owned value that outlives the call, and a stack instance is reclaimed with its frame.

A `stack` allocation cannot bind at script top level — session bindings outlive the entry frame (`CAJETA_ERROR_SESSION_STACK_BINDING`, Script Units §18).

> *Discussion.* **Transfer of `stack` values is TBD** (Ownership §5.3): the transfer machinery is being consolidated, and the semantics of `#` applied to a stack instance beyond the `#`-return rule above are unspecified until that work lands.

## 4.4 Release and Rebinding

An owned value is released by its drop entry. User code never frees directly. A session binding releases its old value at the point the name is rebound (Script Units §18).

> *Discussion.* **Release timing for method-local rebinding is settling** under the ownership consolidation in progress. Re-assigning an owning binding now releases the displaced value and the drop entry follows the new one. The timing of `null`-release and the consolidation's remaining semantics are bound here when that work completes. Until then, scope exit (§4.2) is the guarantee to rely on for method locals.
