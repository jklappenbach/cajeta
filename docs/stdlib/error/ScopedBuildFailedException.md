# ScopedBuildFailedException

`cajeta.error.ScopedBuildFailedException`. Thrown to an injection that waited on another fiber's build of a scoped component when that build failed, to one that re-entered a component while it was being built, and by a second `Components.provide` in one scope. The next injection after a failed build builds again. Extends
[`RecoverableException`](RecoverableException.md).

## Methods

| Signature | |
|---|---|
| `ScopedBuildFailedException(#String message)` | Wrap the message naming the component and the scope |

Inherits the full [`Throwable`](Throwable.md) surface.

## See also

- [Scope](../aot/Scope.md), [Components](../aot/Components.md)
- Source: [`runtime/src/cajeta/error/ScopedBuildFailedException.cajeta`](../../../runtime/src/cajeta/error/ScopedBuildFailedException.cajeta)
