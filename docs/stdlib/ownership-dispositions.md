# stdlib ownership dispositions (plan 1.3.1)

One row per `ownership-audit.md` finding. Checked by
`tools/ownership/audit_ownership.py --check-dispositions <this file>`:
a finding without a row, or a row without a finding, fails the run.
Dispositions: `conforming` | `migrate:<target>` | `exception:<reason>`.

| Disposition | Count |
|---|---|
| conforming | 136 |
| migrate | 40 |

| Kind | File | Method | Type | Disposition | Rationale |
|---|---|---|---|---|---|
| CAPTURE(#=) | `codec/json/JsonLinesWriter.cajeta` | `JsonLinesWriter` | `FileWriter` | conforming | sink-shaped holder; `#=` store carries the caller's lend/transfer choice (§2.3, §2.6 table) |
| CAPTURE(#=) | `collection/CacheNode.cajeta` | `CacheNode` | `K` | conforming | node is the LRU sink; `#=` records the mode `Cache.put` tendered (plain key = borrow, map slot owns it) |
| CAPTURE(#=) | `collection/CacheNode.cajeta` | `CacheNode` | `V` | conforming | node is the value's home; `#=` records the title `Cache.put` transfers (§2.3) |
| CAPTURE(#=) | `collection/LinkedListNode.cajeta` | `LinkedListNode` | `T` | conforming | plain formal + `#=`, so the node owns only what the caller transferred (§2.3) |
| CAPTURE(#=) | `collection/RedBlackNode.cajeta` | `RedBlackNode` | `K` | conforming | tree node is the sink; `#=` keeps the caller's mode per slot (§2.3) |
| CAPTURE(#=) | `collection/RedBlackNode.cajeta` | `RedBlackNode` | `V` | conforming | same sink model, mode recorded not forced (§2.3) |
| CAPTURE(#=) | `concurrent/FiberLocalBox.cajeta` | `FiberLocalBox` | `T` | conforming | box's stated job is holding the binding value; caller chooses via `#=` (§2.3) |
| CAPTURE(#=) | `concurrent/Mutex.cajeta` | `Mutex` | `T` | conforming | Mutex is documented as owning the protected data; `heap Mutex(#v)` vs `(v)` is the caller's call (§2.3) |
| CAPTURE(#=) | `concurrent/RwLock.cajeta` | `RwLock` | `T` | conforming | same fused own-the-data shape as Mutex; ctor doc states both modes (§2.3) |
| CAPTURE(#=) | `concurrent/SelectResult.cajeta` | `SelectResult` | `T` | conforming | result pair is a value carrier; `#=` records the dequeued item's tendered mode (§2.3) |
| CAPTURE(#=) | `io/file/FileEvent.cajeta` | `FileEvent` | `Path` | conforming | same sink; `renameTarget` is null or a fresh watcher-produced path, caller's choice |
| CAPTURE(#=) | `io/net/AsyncReader.cajeta` | `AsyncReader` | `ByteChannel` | conforming | capacity ctor, same shared-channel model |
| CAPTURE(#=) | `io/net/AsyncWriter.cajeta` | `AsyncWriter` | `ByteChannel` | conforming | capacity ctor, same shared-channel model |
| CAPTURE(#=) | `io/net/ServerBuilder.cajeta` | `bindAddress` | `SocketAddress` | conforming | builder holds config until `build()`; callers meaningfully lend a held address or transfer a parsed temp |
| CAPTURE(#=) | `io/net/ServerBuilder.cajeta` | `model` | `ServerModel` | conforming | same sink-shaped config hold; `ServerModel.sharedPool(n)` temps transfer, held models lend |
| CAPTURE(#=) | `lang/Optional.cajeta` | `Optional` | `T` | conforming | sink-shaped wrapper; `#=` plus `take()` for the owned counterpart (§2.3) |
| CAPTURE(#=) | `lang/stream/ArrayStream.cajeta` | `ArrayStream` | `T[]` | conforming | buffer sink; documented as shared-not-copied with mode from the caller (§2.3) |
| CAPTURE(#=) | `math/Tensor.cajeta` | `Tensor` | `Storage<T>` | conforming | §2.3 sink — Tensor's job is holding the buffer, `#=` records the title the factories surrender |
| CAPTURE(#=) | `nucleo/column/Column.cajeta` | `Column` | `Tensor<T>` | conforming | buffer-holding container; `#=` records the caller's mode per slot (§2.3) |
| CAPTURE(#=) | `nucleo/column/MxColumn.cajeta` | `MxColumn` | `Column<uint8>` | conforming | documented mode-forwarding wrapper — both lent and owned packed columns are live (§2.3) |
| CAPTURE(#=) | `nucleo/column/NullableColumn.cajeta` | `NullableColumn` | `Column<T>` | conforming | two-buffer container; `#=` values slot carries the source's mode (§2.3) |
| CAPTURE(#=) | `nucleo/column/NullableColumn.cajeta` | `NullableColumn` | `Column<uint8>` | conforming | same sink, validity bitmap slot (§2.3) |
| CAPTURE(#=) | `nucleo/column/StringColumn.cajeta` | `StringColumn` | `Column<int32>` | conforming | offsets buffer held by a buffer container, `#=` store (§2.3) |
| CAPTURE(#=) | `nucleo/column/StringColumn.cajeta` | `StringColumn` | `Column<uint8>` | conforming | utf8 data buffer, same sink slot (§2.3) |
| CAPTURE(#=) | `nucleo/transform/GradResult.cajeta` | `GradResult` | `G` | conforming | record result-carrier whose job is holding the returned grads (§2.3) |
| CAPTURE(#=) | `nucleo/transform/GradResult.cajeta` | `GradResult` | `V` | conforming | same record carrier, forward-value slot (§2.3) |
| CAPTURE(#=) | `search/fuzzy/Match.cajeta` | `Match` | `T` | conforming | §2.3 sink-shaped carrier, `#=` carries whichever mode the value arrived in |
| CAPTURE(=) | `codec/Base64Exception.cajeta` | `Base64Exception` | `String` | migrate:#String param | non-sink plain store of message (§2.4); base RecoverableException already takes `#String` |
| CAPTURE(=) | `codec/csv/CsvParseException.cajeta` | `CsvParseException` | `String` | migrate:#String param | same class as above; align with base-class transfer-in convention |
| CAPTURE(=) | `codec/json/JsonParseException.cajeta` | `JsonParseException` | `String` | migrate:#String param | same class as above; message escapes with the throw, plain store dangles |
| CAPTURE(=) | `error/ClassCastException.cajeta` | `ClassCastException` | `String` | migrate:#String param | non-sink capture of a plain `String` beyond the call (§2.4); every sibling (`Throwable`, `Exception`, `RecoverableException`, `UnrecoverableException`, `NoOptionalValueException`) already takes `#String` |
| CAPTURE(=) | `ifx/IfxException.cajeta` | `IfxException` | `String` | migrate:#String message | §2.4 capture; base `RecoverableException(#String)` already takes title — subclass diverges |
| CAPTURE(=) | `io/net/AddressInUseException.cajeta` | `AddressInUseException` | `String` | migrate:#String param | non-sink keeps the message past the throw site (§2.4); base `Throwable(#String)` already spells it so |
| CAPTURE(=) | `io/net/AddressNotAvailableException.cajeta` | `AddressNotAvailableException` | `String` | migrate:#String param | same §2.4 capture; matches the DNS siblings' `#String` |
| CAPTURE(=) | `io/net/BrokenPipeException.cajeta` | `BrokenPipeException` | `String` | migrate:#String param | same §2.4 capture |
| CAPTURE(=) | `io/net/ConnectionAbortedException.cajeta` | `ConnectionAbortedException` | `String` | migrate:#String param | same §2.4 capture |
| CAPTURE(=) | `io/net/ConnectionRefusedException.cajeta` | `ConnectionRefusedException` | `String` | migrate:#String param | same §2.4 capture |
| CAPTURE(=) | `io/net/ConnectionResetException.cajeta` | `ConnectionResetException` | `String` | migrate:#String param | same §2.4 capture |
| CAPTURE(=) | `io/net/HostUnreachableException.cajeta` | `HostUnreachableException` | `String` | migrate:#String param | same §2.4 capture |
| CAPTURE(=) | `io/net/MalformedAddressException.cajeta` | `MalformedAddressException` | `String` | migrate:#String param | `(message, position)` ctor stores the message in a field that outlives the throw |
| CAPTURE(=) | `io/net/NetException.cajeta` | `NetException` | `String` | migrate:#String param | two-arg `(message, kind)` ctor, same capture |
| CAPTURE(=) | `io/net/NetworkUnreachableException.cajeta` | `NetworkUnreachableException` | `String` | migrate:#String param | same §2.4 capture |
| CAPTURE(=) | `io/net/TimedOutException.cajeta` | `TimedOutException` | `String` | migrate:#String param | same §2.4 capture |
| CAPTURE(=) | `io/net/TooManyConnectionsException.cajeta` | `TooManyConnectionsException` | `String` | migrate:#String param | `(message, cap)` ctor stores the message beyond the call |
| CAPTURE(=) | `io/net/WouldBlockException.cajeta` | `WouldBlockException` | `String` | migrate:#String param | same §2.4 capture even on the value-surfaced hot path |
| CAPTURE(=) | `io/net/tls/CertificateInvalidException.cajeta` | `CertificateInvalidException` | `String` | migrate:#String param | `(message, reason)` ctor stores the message beyond the call |
| CAPTURE(=) | `io/net/tls/TlsException.cajeta` | `TlsException` | `String` | migrate:#String param | same §2.4 capture as the net roots |
| CAPTURE(=) | `io/net/uri/MalformedUriException.cajeta` | `MalformedUriException` | `String` | migrate:#String param | `(message, position)` ctor stores the message beyond the call |
| CAPTURE(=) | `math/BroadcastException.cajeta` | `BroadcastException` | `String` | migrate:#String message | §2.4 capture; diverges from `Throwable(#String message)` |
| CAPTURE(=) | `math/PlacementException.cajeta` | `PlacementException` | `String` | migrate:#String message | §2.4 capture; diverges from `Throwable(#String message)` |
| CAPTURE(=) | `math/distance/DistanceException.cajeta` | `DistanceException` | `String` | migrate:#String message | §2.4 capture; diverges from `Throwable(#String message)` |
| CAPTURE(=) | `math/linalg/LinAlgException.cajeta` | `LinAlgException` | `String` | migrate:#String message | §2.4 capture; diverges from `Throwable(#String message)` |
| CAPTURE(=) | `math/optim/OptimException.cajeta` | `OptimException` | `String` | migrate:#String message | §2.4 capture; diverges from `Throwable(#String message)` |
| CAPTURE(=) | `math/stats/StatsException.cajeta` | `StatsException` | `String` | migrate:#String message | §2.4 capture; diverges from `Throwable(#String message)` |
| CAPTURE(=) | `nucleo/column/ColumnTypeException.cajeta` | `ColumnTypeException` | `String` | migrate:#String param | plain capture of a message held for the throwable's life; base `RecoverableException(#String)` already spells it (§2.4) |
| CAPTURE(=) | `nucleo/frame/DynFrame.cajeta` | `setSpatial` | `String` | migrate:#= store | same call, y-axis name field, carried across `alias` (§2.4→§2.3) |
| CAPTURE(=) | `nucleo/frame/FrameException.cajeta` | `FrameException` | `String` | migrate:#String param | plain capture into `message`; diverges from the `#String` base ctor (§2.4) |
| CAPTURE(=) | `nucleo/sparse/SparseException.cajeta` | `SparseException` | `String` | migrate:#String param | plain capture into `message`; diverges from the `#String` base ctor (§2.4) |
| CAPTURE(=) | `search/fuzzy/Match.cajeta` | `Match` | `String` | migrate:#String key (+#= store) | §2.4 and a live defect — `Matcher` passes `#kc` into a plain formal that frees at ctor exit |
| CAPTURE(=) | `time/DateTimeException.cajeta` | `DateTimeException` | `String` | migrate:#String message | §2.4 capture; diverges from `Throwable(#String message)` |
| CAPTURE(=) | `time/DateTimeFormatter.cajeta` | `DateTimeFormatter` | `String` | migrate:copy | §2.5 — the formatter keeps a pattern it was only shown; copy is the safe unmarked spelling |
| CAPTURE(=) | `time/DateTimeParseException.cajeta` | `DateTimeParseException` | `String` | migrate:#String message | §2.4 capture; diverges from `Throwable(#String message)` |
| CAPTURE(=) | `time/ZoneId.cajeta` | `ZoneId` | `String` | migrate:copy + ofBorrowed alias | §2.5 — doc's "strings are process-lifetime" holds only for literals, so copy by default and name the alias |
| CAPTURE(elem) | `collection/ArrayList.cajeta` | `add` | `T` | conforming | the canonical §2.3 sink: plain formal, `#=` slot store, per-slot mode |
| CAPTURE(elem) | `collection/ArrayList.cajeta` | `insert` | `T` | conforming | same plain-formal `#=` store, shifts forward each slot's bit (§2.3) |
| CAPTURE(elem) | `collection/ArrayList.cajeta` | `set` | `T` | conforming | `#=` store with displaced release only when the old slot held title (§2.3) |
| CAPTURE(elem) | `collection/Heap.cajeta` | `push` | `T` | conforming | plain formal, `#=` store, bits ride the sift-up swaps (§2.3) |
| CAPTURE(elem) | `math/Storage.cajeta` | `set` | `T` | migrate:#= element store | §2.3 sink stores per-slot mode; `this.host[i] = v` diverges from `ArrayList`'s `this.data[i] #= v` |
| CAPTURE(elem) | `nucleo/frame/DynFrame.cajeta` | `addIndexed` | `String` | conforming | already `#=` per element with the mode recorded in the slot (§2.3) |
| CONDITIONAL | `codec/json/JsonValue.cajeta` | `setString` | `JsonValue` | migrate:copy + setStringBorrowed alias | branches on `s.root()`/`byteOffset()` — invisible to caller (§2.6); safe spelling becomes unmarked (§2.5, plan 4.2.1) |
| PRODUCER? | `codec/json/JsonValue.cajeta` | `asArray` | `JsonArray` | migrate:lifetime doc + §2.7 name review | body is a pure interior read (view) but `as...` reads as producer (§2.7); rename vs doc-only decided in 4.2.3 |
| PRODUCER? | `codec/json/JsonValue.cajeta` | `asObject` | `JsonObject` | migrate:lifetime doc + §2.7 name review | same as asArray |
| VIEW-RETURN | `buildtool/plugin/ActionResult.cajeta` | `errorMessage` | `String` | conforming | body is a bare field read; caller copies to outlive the result (§2.2) |
| VIEW-RETURN | `buildtool/plugin/ActionResult.cajeta` | `findings` | `ArrayList<Finding>` | conforming | bare interior read of the owned list |
| VIEW-RETURN | `buildtool/plugin/ActionResult.cajeta` | `outputs` | `HashMap<String, String>` | conforming | bare interior read of the owned map |
| VIEW-RETURN | `buildtool/plugin/Finding.cajeta` | `file` | `String` | conforming | bare field read of a `#`-captured string |
| VIEW-RETURN | `buildtool/plugin/Finding.cajeta` | `message` | `String` | conforming | bare field read |
| VIEW-RETURN | `buildtool/plugin/Finding.cajeta` | `rule` | `String` | conforming | bare field read |
| VIEW-RETURN | `codec/json/JsonArray.cajeta` | `get` | `JsonValue` | conforming | `return this.data[i]` — interior read only (§2.2) |
| VIEW-RETURN | `codec/json/JsonObject.cajeta` | `get` | `JsonValue` | conforming | delegates to indexed lookup; returns interior element (§2.2) |
| VIEW-RETURN | `codec/json/JsonObject.cajeta` | `valueAt` | `JsonValue` | conforming | `return this.values[i]` — interior read only (§2.2) |
| VIEW-RETURN | `collection/ArrayList.cajeta` | `get` | `T` | conforming | body is `return this.data[i]` — interior read only, always a borrow (§2.2) |
| VIEW-RETURN | `collection/HashMap.cajeta` | `get` | `V` | conforming | returns `this.slots[i].val` plus a zero miss-default; no `#=` extraction (§2.2) |
| VIEW-RETURN | `collection/Heap.cajeta` | `peek` | `T` | conforming | `return this.data[0]` / zero default; interior read only (§2.2) |
| VIEW-RETURN | `collection/ImmutableList.cajeta` | `get` | `T` | conforming | interior read of `this.data[i]` with a zero miss-default (§2.2) |
| VIEW-RETURN | `collection/ImmutableMap.cajeta` | `get` | `V` | conforming | SIMD probe then `return this.valArr[ei]` — read only (§2.2) |
| VIEW-RETURN | `collection/ImmutableMap.cajeta` | `keyAt` | `K` | conforming | dense-index interior read; `...At` name reads as a view (§2.2, §2.7) |
| VIEW-RETURN | `collection/ImmutableMap.cajeta` | `valAt` | `V` | conforming | dense-index interior read (§2.2, §2.7) |
| VIEW-RETURN | `collection/ImmutableSet.cajeta` | `get` | `T` | conforming | `return this.elements[i]` with a zero miss-default (§2.2) |
| VIEW-RETURN | `collection/graph/Digraph.cajeta` | `engine` | `IndexGraph` | conforming | `return this.core`; doc already states "(borrow)" (§2.2, §2.7) |
| VIEW-RETURN | `error/Throwable.cajeta` | `getMessage` | `String` | conforming | `return this.message` — interior read, ownership stays with the throwable (§2.2) |
| VIEW-RETURN | `ifx/IfxInfo.cajeta` | `audioBackendName` | `String` | conforming | §2.2 — body is `return this.audioName` only |
| VIEW-RETURN | `ifx/IfxInfo.cajeta` | `inputBackendName` | `String` | conforming | §2.2 — body is `return this.inputName` only |
| VIEW-RETURN | `ifx/IfxInfo.cajeta` | `windowBackendName` | `String` | conforming | §2.2 — body is `return this.windowName` only |
| VIEW-RETURN | `io/net/Headers.cajeta` | `nameAt` | `String` | conforming | returns `this.keys[i]` only; `...At` reads as a view per §2.7 |
| VIEW-RETURN | `io/net/Headers.cajeta` | `valueAt` | `String` | conforming | returns `this.values[i]` only |
| VIEW-RETURN | `io/net/RecvResult.cajeta` | `getFrom` | `SocketAddress` | conforming | bare read of the address the result took title to |
| VIEW-RETURN | `io/net/SocketAddress.cajeta` | `getIp` | `IpAddress` | conforming | bare field read |
| VIEW-RETURN | `io/net/uri/Uri.cajeta` | `getFragment` | `String` | conforming | bare field read of a parsed component |
| VIEW-RETURN | `io/net/uri/Uri.cajeta` | `getHost` | `String` | conforming | bare field read |
| VIEW-RETURN | `io/net/uri/Uri.cajeta` | `getPath` | `String` | conforming | bare field read |
| VIEW-RETURN | `io/net/uri/Uri.cajeta` | `getQuery` | `String` | conforming | bare field read (raw, still percent-encoded) |
| VIEW-RETURN | `io/net/uri/Uri.cajeta` | `getScheme` | `String` | conforming | bare field read |
| VIEW-RETURN | `io/net/uri/Uri.cajeta` | `getUserinfo` | `String` | conforming | bare field read |
| VIEW-RETURN | `lang/Optional.cajeta` | `get` | `T` | conforming | `return this.value` after the present check; doc names it a borrow and points at `take()` for title (§2.2, §2.7) |
| VIEW-RETURN | `lang/Optional.cajeta` | `orElse` | `T` | conforming | both arms are borrows — interior read or the caller's own plain `fallback` (§2.2) |
| VIEW-RETURN | `lang/Pair.cajeta` | `first` | `K` | conforming | `return this.first`; `takeFirst()` is the separate owning variant (§2.2, §2.5) |
| VIEW-RETURN | `lang/Pair.cajeta` | `second` | `V` | conforming | `return this.second`; `takeSecond()` is the owning counterpart (§2.2, §2.5) |
| VIEW-RETURN | `lang/stream/FilterStream.cajeta` | `unwrap` | `Stream<?>` | conforming | `return this.source` — interior read for the chain walker, no title (§2.2) |
| VIEW-RETURN | `lang/stream/FlatMapStream.cajeta` | `unwrap` | `Stream<?>` | conforming | `return this.source`, interior read only (§2.2) |
| VIEW-RETURN | `lang/stream/MapOrFallbackStream.cajeta` | `unwrap` | `Stream<?>` | conforming | `return this.source`, interior read only (§2.2) |
| VIEW-RETURN | `lang/stream/MapOrLogStream.cajeta` | `unwrap` | `Stream<?>` | conforming | `return this.source`, interior read only (§2.2) |
| VIEW-RETURN | `lang/stream/MapOrSkipStream.cajeta` | `unwrap` | `Stream<?>` | conforming | `return this.source`, interior read only (§2.2) |
| VIEW-RETURN | `lang/stream/MapStream.cajeta` | `unwrap` | `Stream<?>` | conforming | `return this.source`; the owning rebuild is the separate `cloneChainOver` (§2.2) |
| VIEW-RETURN | `lang/stream/PeekStream.cajeta` | `unwrap` | `Stream<?>` | conforming | `return this.source`, interior read only (§2.2) |
| VIEW-RETURN | `lang/stream/SkipStream.cajeta` | `unwrap` | `Stream<?>` | conforming | `return this.source`, interior read only (§2.2) |
| VIEW-RETURN | `lang/stream/TakeStream.cajeta` | `unwrap` | `Stream<?>` | conforming | `return this.source`, interior read only (§2.2) |
| VIEW-RETURN | `math/Storage.cajeta` | `deviceBuffer` | `KernelBuffer<T>` | conforming | §2.2 — body is `return this.dev` only; name reads as an accessor, not a producer |
| VIEW-RETURN | `math/Storage.cajeta` | `get` | `T` | conforming | §2.2 — the canonical `get(i)` interior read (after a residency guard) |
| VIEW-RETURN | `math/Tensor.cajeta` | `base` | `Tensor<T>` | conforming | §2.2 — body is `return this.baseTensor` only |
| VIEW-RETURN | `math/TensorProtocol.cajeta` | `base` | `Object` | conforming | §2.2 — interior read, doc states the storage is borrowed |
| VIEW-RETURN | `math/TensorProtocol.cajeta` | `dtype` | `DType` | conforming | §2.2 — body is `return this.dt` only |
| VIEW-RETURN | `nucleo/frame/Agg.cajeta` | `exprOf` | `ColF64` | conforming | body is a bare field read, always borrow (§2.2) |
| VIEW-RETURN | `nucleo/frame/Agg.cajeta` | `iRefOf` | `ColI64` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/Agg.cajeta` | `leftAgg` | `Agg` | conforming | bare field read of the child link (§2.2) |
| VIEW-RETURN | `nucleo/frame/Agg.cajeta` | `nextOf` | `Agg` | conforming | bare field read of the chain link (§2.2) |
| VIEW-RETURN | `nucleo/frame/Agg.cajeta` | `outNameOf` | `String` | conforming | bare field read; `...Of` name reads as a view (§2.2, §2.7) |
| VIEW-RETURN | `nucleo/frame/Agg.cajeta` | `rightAgg` | `Agg` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/Agg.cajeta` | `strRefOf` | `ColStr` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/ColF64.cajeta` | `leftChild` | `ColF64` | conforming | bare field read of the expression child (§2.2) |
| VIEW-RETURN | `nucleo/frame/ColF64.cajeta` | `name` | `String` | conforming | bare field read of the column name (§2.2) |
| VIEW-RETURN | `nucleo/frame/ColF64.cajeta` | `rightChild` | `ColF64` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/ColI64.cajeta` | `name` | `String` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/ColStr.cajeta` | `name` | `String` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/DynFrame.cajeta` | `colAt` | `DynCol` | conforming | indexed read of the interior column array (§2.2) |
| VIEW-RETURN | `nucleo/frame/DynFrame.cajeta` | `indexedAt` | `String` | conforming | indexed interior read; `...At` reads as a view (§2.2, §2.7) |
| VIEW-RETURN | `nucleo/frame/DynFrame.cajeta` | `nameAt` | `String` | conforming | indexed interior read (§2.2) |
| VIEW-RETURN | `nucleo/frame/DynFrame.cajeta` | `spatialXOf` | `String` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/DynFrame.cajeta` | `spatialYOf` | `String` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/Plan.cajeta` | `aggsOf` | `Agg` | conforming | bare field read of the aggregate chain head (§2.2) |
| VIEW-RETURN | `nucleo/frame/Plan.cajeta` | `childOf` | `Plan` | conforming | bare field read of the input node (§2.2) |
| VIEW-RETURN | `nucleo/frame/Plan.cajeta` | `colNameOf` | `String` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/Plan.cajeta` | `joinKeysOf` | `Sel` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/Plan.cajeta` | `joinOutOf` | `DynFrame` | conforming | bare field read of the cached join schema (§2.2) |
| VIEW-RETURN | `nucleo/frame/Plan.cajeta` | `keysOf` | `Sel` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/Plan.cajeta` | `meltIdAt` | `String` | conforming | indexed interior read (§2.2) |
| VIEW-RETURN | `nucleo/frame/Plan.cajeta` | `meltValAt` | `String` | conforming | indexed interior read (§2.2) |
| VIEW-RETURN | `nucleo/frame/Plan.cajeta` | `meltValNameOf` | `String` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/Plan.cajeta` | `meltVarNameOf` | `String` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/Plan.cajeta` | `predOf` | `Pred` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/Plan.cajeta` | `rightPlanOf` | `Plan` | conforming | bare field read of the right branch (§2.2) |
| VIEW-RETURN | `nucleo/frame/Plan.cajeta` | `rightSrcOf` | `DynFrame` | conforming | bare field read of the right scan snapshot (§2.2) |
| VIEW-RETURN | `nucleo/frame/Plan.cajeta` | `selsOf` | `Sel` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/Plan.cajeta` | `sortKeysOf` | `SortKey` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/Pred.cajeta` | `iOperandExpr` | `ColI64` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/Pred.cajeta` | `leftPredicate` | `Pred` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/Pred.cajeta` | `operandExpr` | `ColF64` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/Pred.cajeta` | `rightPredicate` | `Pred` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/Pred.cajeta` | `strOperandExpr` | `ColStr` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/Pred.cajeta` | `strValue` | `String` | conforming | bare field read of the literal operand (§2.2) |
| VIEW-RETURN | `nucleo/frame/Sel.cajeta` | `exprOf` | `ColF64` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/Sel.cajeta` | `iRefOf` | `ColI64` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/Sel.cajeta` | `nextOf` | `Sel` | conforming | bare field read of the chain link (§2.2) |
| VIEW-RETURN | `nucleo/frame/Sel.cajeta` | `outNameOf` | `String` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/Sel.cajeta` | `strRefOf` | `ColStr` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/SortKey.cajeta` | `exprOf` | `ColF64` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/SortKey.cajeta` | `iRefOf` | `ColI64` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/SortKey.cajeta` | `nextOf` | `SortKey` | conforming | bare field read of the chain link (§2.2) |
| VIEW-RETURN | `nucleo/frame/SortKey.cajeta` | `strRefOf` | `ColStr` | conforming | bare field read (§2.2) |
| VIEW-RETURN | `nucleo/frame/Table.cajeta` | `collect` | `Table<T>` | conforming | returns `this` or the interior `cached` handle — always a borrow, and the cache-on-force contract requires the handle keep title (§2.2) |
| VIEW-RETURN | `search/fuzzy/Match.cajeta` | `key` | `String` | conforming | §2.2 — body is `return this.key` only; caller copies to outlive the match |
| VIEW-RETURN | `search/fuzzy/Match.cajeta` | `value` | `T` | conforming | §2.2 — body is `return this.value` only |
| VIEW-RETURN | `time/DateTimeFormatter.cajeta` | `getPattern` | `String` | conforming | §2.2 — body is `return this.pattern` only |
| VIEW-RETURN | `time/ZoneId.cajeta` | `getId` | `String` | conforming | §2.2 — body is `return this.id` only |
| VIEW-RETURN | `xpu/PageCache.cajeta` | `evictedKey` | `K` | conforming | §2.2/§2.7 — interior read, and the doc states the validity bound |
| VIEW-RETURN | `xpu/PageCache.cajeta` | `getOrDefault` | `V` | conforming | §2.2 — returns either the caller's own `fallback` or an interior slot read |
