# NotProvidedException

`cajeta.error.NotProvidedException`. Thrown when a scoped component with no no-argument constructor is injected before `Components.provide` placed it in the active scope. The message names the component and the scope. Extends
[`RecoverableException`](RecoverableException.md).

## Methods

| Signature | |
|---|---|
| `NotProvidedException(#String message)` | Wrap the message naming the component and the scope |

Inherits the full [`Throwable`](Throwable.md) surface.

## See also

- [Scope](../aot/Scope.md), [Components](../aot/Components.md)
- Source: [`runtime/src/cajeta/error/NotProvidedException.cajeta`](../../../runtime/src/cajeta/error/NotProvidedException.cajeta)
