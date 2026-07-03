# LinkedList\<T\>

`cajeta.collection.LinkedList` — doubly-linked list of `T`. The head/tail API
makes it usable as a deque or a stack: push/pop at either end in O(1). It only
needs `==` for `remove` / `contains`, so both primitive `T` and class `T`
(identity comparison) work. Peek/pop on an empty list return the type's zero
value; guard with `count()` when the zero value is itself a valid element.

```cajeta
LinkedList<int64> nums = heap LinkedList<int64>();
nums.addTail(10);      // tail:  [10]
nums.addTail(20);      // tail:  [10, 20]
nums.addHead(5);       // head:  [5, 10, 20]
int64 front = nums.head();        // 5  (peek, no removal)
int64 back  = nums.tail();        // 20
int64 first = nums.popHead();     // 5  -> [10, 20]
int64 last  = nums.popTail();     // 20 -> [10]
boolean has = nums.contains(10);  // true
int64 n = nums.count();           // 1
```

## Methods

| Signature | |
|---|---|
| `LinkedList()` ⚑ | Create an empty list |
| `void add(T value)` | Append `value` to the tail (alias of `addTail`) |
| `void addFirst(T value)` | Prepend `value` to the head (alias of `addHead`) |
| `void addTail(T value)` | Append `value` to the tail |
| `void addHead(T value)` | Prepend `value` to the head |
| `T head()` | Peek the value at the head without removing it |
| `T tail()` | Peek the value at the tail without removing it |
| `T popHead()` | Remove and return the value at the head |
| `T popTail()` | Remove and return the value at the tail |
| `T get(int64 idx)` | Value at index `idx` (0-based), walking from the closer end |
| `boolean remove(T value)` | Unlink the first `==`-matching value |
| `boolean contains(T value)` | Test whether `value` is present |
| `int64 count()` | Live node count |

⚑ = `@EntryPoint`

## See also

- Tour: [LinkedListDemo](../../../samples/tour/src/main/cajeta/tour/collection/LinkedListDemo.cajeta)
- Source: [`runtime/src/cajeta/collection/LinkedList.cajeta`](../../../runtime/src/cajeta/collection/LinkedList.cajeta)
- [ArrayList](ArrayList.md) — the index-addressable alternative
