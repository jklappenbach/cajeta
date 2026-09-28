# ScopeNotActiveException

`cajeta.error.ScopeNotActiveException`. Thrown when a scoped component is injected, provided, or its scope entered through `within`, while no activation of the scope it needs is active. The message names the component and the scope. Extends
[`RecoverableException`](RecoverableException.md).

## Methods

| Signature | |
|---|---|
| `ScopeNotActiveException(#String message)` | Wrap the message naming the component and the scope |

Inherits the full [`Throwable`](Throwable.md) surface.

## See also

- [Scope](../aot/Scope.md), [Components](../aot/Components.md)
- Source: [`runtime/src/cajeta/error/ScopeNotActiveException.cajeta`](../../../runtime/src/cajeta/error/ScopeNotActiveException.cajeta)
