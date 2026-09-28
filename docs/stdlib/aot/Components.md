# Components

`cajeta.aot.Components` places values produced at run time into the DI
graph's scopes. A scoped component with no no-argument constructor is only
ever provided this way.

```cajeta
import cajeta.aot.Components;

@Component(scope = "Login")
public class User {
    public int32 id;
    public User(int32 id) { this.id = id; return; }
}

public final class Gate {
    @Scope("Login")
    public static int32 enter(int32 id) {
        User u = heap User(id);
        Components.provide(#u);
        User again = User.__cajeta_inject();
        return again.id;
    }
}
```

## Methods

| Signature | |
|---|---|
| `static void provide<T>(#T value)` | Place `value` in the active scope `T` declares |

The scope owns a transferred value and frees it at its end. A second provide
in one scope throws [`ScopedBuildFailedException`](../error/ScopedBuildFailedException.md)
and frees the refused value. Outside every activation of the scope it throws
[`ScopeNotActiveException`](../error/ScopeNotActiveException.md). Injecting a
provided-only type before it is provided throws
[`NotProvidedException`](../error/NotProvidedException.md).

## See also

- [Scope](Scope.md), [Scoped](Scoped.md)
- Source: [`runtime/src/cajeta/aot/Components.cajeta`](../../../runtime/src/cajeta/aot/Components.cajeta)
