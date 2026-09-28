# Scope

`cajeta.aot.Scope` publishes a named component lifetime at the code that
bounds it. Components consume it by name with `@Component(scope = "Name")`.
Where it is placed decides the kind of scope:

| Placed on | The scope is | It ends |
|---|---|---|
| a method, or a method it overrides or implements | one activation | on return or throw |
| a class | one instance, current while a non-private instance method runs | when the instance is dropped |

```cajeta
public final class Worker {
    @Scope("Job")
    public static int32 run(int32 n) {
        Progress p = Progress.__cajeta_inject();
        p.done = p.done + n;
        return p.done;
    }
}

@Component(scope = "Job")
public class Progress {
    public int32 done;
    public Progress() { this.done = 0; return; }
}
```

## Members

| Member | |
|---|---|
| `String value()` | The published name. A consumer qualifies it by package only on a clash |
| `String within()` | A published scope this one is always entered inside, checked on entry |

## Rules

- Several methods in one package may publish one name. They must agree on
  its kind and its `within`.
- `@Scope` on a field, a constructor, a record or an interface type is a
  compile error. On an interface method it makes every implementation an
  anchor.
- `"Singleton"` and `"Transient"` are built in and cannot be published.

## See also

- [Scoped](Scoped.md), [Components](Components.md)
- [ScopeNotActiveException](../error/ScopeNotActiveException.md)
- Guide: [DI & aspects § Scopes](../../guide/19-di-aspects.md)
- Source: [`runtime/src/cajeta/aot/Scope.cajeta`](../../../runtime/src/cajeta/aot/Scope.cajeta)
