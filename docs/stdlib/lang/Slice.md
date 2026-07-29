# Slice\<T\>

`cajeta.lang.Slice` — the array-window value type: `arr[a:b]` yields a
`Slice<T>`. Three machine words passed and copied by value — `{ store, off,
len }` — with no per-slice heap object: the window shares the array's root
buffer zero-copy. There is no public constructor; slices are compiler-built
from the `arr[a:b]` expression. Sub-slicing (`s[a:b]`) composes offsets
against the root in O(1). Indexing is window-relative and checked against the
window `[0, len)`; the root array's own bounds check still guards the buffer.

```cajeta
int64[] arr = heap int64[16];
int32 i = 0;
while (i < 16) { arr[i] = (int64) (i * 2); i = i + 1; }
Slice<int64> s = arr[4:12];      // window over arr[4..12), zero-copy
int64 n = s.count();             // 8
int64 first = s[0];              // arr[4] == 8
Slice<int64> t = s[2:6];         // sub-slice: arr[6..10), composes to the root
```

## Methods

| Signature | |
|---|---|
| `int64 count()` ⚑ | Window length — independent of the backing buffer's capacity |
| `boolean isEmpty()` | True when the window is empty |
| `T operator[] (int64 idx)` ⚑ | Window-relative element read; out-of-window aborts |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/lang/Slice.cajeta`](../../../runtime/src/cajeta/lang/Slice.cajeta)
- [String](String.md) — `substring`/`trim` apply the same zero-copy windowing to text
