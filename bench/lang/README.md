# Language and cajeta.lang

The language itself, measured bare, then the classes of `cajeta.lang`. Loops, exceptions, allocation and dispatch are the floors; everything else on this site is built on them.

| Page | |
|---|---|
| [Loops](Loops.md) | Counted, nested, foreach and range loops, reductions and early exit: the floors under every other number |
| [Exceptions](Exceptions.md) | A try that never throws, and throws caught across one, ten and a hundred frames, and inside a fiber |
| [Allocation and ownership](Allocation.md) | Heap allocation and scope drop, churn, ownership transfer, the live-allocation set on and off |
| [Calls and dispatch](Dispatch.md) | Direct, virtual, interface, template, lambda and method-reference calls, and instanceof |
| [String](String.md) | Immutable UTF-8 text, the single string type |
| [StringBuilder](StringBuilder.md) | The growable byte buffer that builds a String in amortized O |
| [Optional](Optional.md) | The value-typed sum, present or empty |
| [Pair](Pair.md) | The two-field template value type |
| [Slice](Slice.md) | The zero-copy array window behind arr[a:b] |
| [Math and SIMD](Math.md) | Numeric utilities on the host, plus the Vector<T,N> SIMD type the two oldest rows use |
| [Guid](Guid.md) | The 128-bit UUID held in one uint128 |
| [Stream](stream/Stream.md) | The pull-protocol base for stdlib iteration |
| [ArrayStream](stream/ArrayStream.md) | A Stream over a contiguous T[] range, the stream every array and ArrayList hands out |
| [Whole programs: the Benchmarks Game](Programs.md) | Three Computer Language Benchmarks Game problems at their canonical sizes, with the published checksums verified: mandelbrot, fannkuch-redux and spectral-norm |

## Standing

Written after the first published run.
