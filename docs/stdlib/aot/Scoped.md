# Scoped

`cajeta.aot.Scoped<T>` is a handle on a scoped component, injected where the
holder outlives the component's scope. A field may hold only a component that
outlives its holder, so a singleton that needs a request's component injects
the handle and resolves it where it is needed.

```cajeta
import cajeta.aot.Scoped;

@Component(scope = "Order")
public class Budget {
    public int32 left;
    public Budget() { this.left = 10; return; }
}

@Component
public class Spender {
    @Inject Scoped<Budget> budget;
    public Spender() { return; }
    public int32 spend(int32 n) {
        Budget b = this.budget.get();
        b.left = b.left - n;
        return b.left;
    }
}

public final class Calls {
    @Scope("Order")
    public static int32 once(int32 n) {
        Spender s = Spender.__cajeta_inject();
        return s.spend(n);
    }
}
```

## Methods

| Signature | |
|---|---|
| `T get()` | The component in the scope active now, borrowed from its scope |

`get()` outside every activation of the scope throws
[`ScopeNotActiveException`](../error/ScopeNotActiveException.md). The holder
owns the handle, and the handle owns nothing.

## See also

- [Scope](Scope.md), [Components](Components.md)
- Source: [`runtime/src/cajeta/aot/Scoped.cajeta`](../../../runtime/src/cajeta/aot/Scoped.cajeta)
