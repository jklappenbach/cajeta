# Stdlib reference

One document per public stdlib class: purpose, the `@EntryPoint`-tagged
methods, and the full public method surface, verified against
`runtime/src/cajeta/`. The [guide](../guide/README.md) is the linear
tutorial; it links here for API detail. Docs link the
[tour](../../samples/tour/) demo that exercises each class, where one exists.

## cajeta.lang

| Class | |
|---|---|
| [String](lang/String.md) | Immutable UTF-8 text, the single string type |
| [StringBuilder](lang/StringBuilder.md) | Growable byte buffer for building a String in amortized O(N) |
| [Optional](lang/Optional.md) | Value-typed sum: present or empty |
| [Pair](lang/Pair.md) | Two-field generic value type |
| [Math](lang/Math.md) | Numeric utilities + device math intrinsics |
| [Guid](lang/Guid.md) | 128-bit UUID backed by a single `uint128` |
| [Slice](lang/Slice.md) | Zero-copy array window, the `arr[a:b]` result type |

## cajeta.lang.stream

| Class | |
|---|---|
| [Stream](lang/stream/Stream.md) | The pull-protocol base for stdlib iteration |
| [ArrayStream](lang/stream/ArrayStream.md) | A `Stream` over a contiguous `T[]` range |

## cajeta.collection

| Class | |
|---|---|
| [ArrayList](collection/ArrayList.md) | Growable index-addressable sequence; the workhorse list |
| [LinkedList](collection/LinkedList.md) | Doubly-linked list |
| [HashMap](collection/HashMap.md) | SwissTable-backed hash map over a dense entry array |
| [HashSet](collection/HashSet.md) | Hash-based set of unique values |
| [ImmutableList](collection/ImmutableList.md) | Immutable array-backed list |
| [ImmutableMap](collection/ImmutableMap.md) | Frozen hash-indexed map snapshot |
| [ImmutableSet](collection/ImmutableSet.md) | Frozen hash-indexed set |
| [Heap](collection/Heap.md) | Array-backed binary heap / priority queue |
| [RedBlackTree](collection/RedBlackTree.md) | Ordered map, CLRS red-black tree |
| [BPlusTree](collection/BPlusTree.md) | In-memory ordered map, B+ tree leaves in a linked chain |
| [Cache](collection/Cache.md) | Bounded cache with LRU eviction and optional TTL |
| [Collectors](collection/Collectors.md) | Standard `Collector` factories for stream terminal ops |
| [Sort](collection/Sort.md) | General-purpose host sorting + shared comparison protocol |

## cajeta.collection.ltm

| Class | |
|---|---|
| [LtmBPlusTree](collection/ltm/LtmBPlusTree.md) | Disk-backed larger-than-memory ordered map |

## cajeta.concurrent

| Class | |
|---|---|
| [Mutex](concurrent/Mutex.md) | Fused mutual exclusion + protected data |
| [RwLock](concurrent/RwLock.md) | Reader-writer lock for read-heavy shared state |
| [Lock](concurrent/Lock.md) | No-data RAII gate |
| [Semaphore](concurrent/Semaphore.md) | Counting permit pool |
| [Channel](concurrent/Channel.md) | Bounded MPMC queue |
| [AtomicInt32](concurrent/AtomicInt32.md) | Lock-free atomic `int32` cell |
| [AtomicInt64](concurrent/AtomicInt64.md) | Lock-free atomic `int64` cell |
| [FiberLocal](concurrent/FiberLocal.md) | Ambient per-request state |
| [Tasks](concurrent/Tasks.md) | Task utilities |

## cajeta.time

| Class | |
|---|---|
| [Clock](time/Clock.md) | The static "what time is it" surface |
| [Instant](time/Instant.md) | A moment on the UTC timeline, nanosecond precision |
| [Duration](time/Duration.md) | Signed time-based amount, nanosecond precision |
| [Period](time/Period.md) | Calendar-based amount: years, months, days |
| [LocalDate](time/LocalDate.md) | Calendar date, no time-of-day or zone |
| [LocalTime](time/LocalTime.md) | Time-of-day, no date or zone |
| [LocalDateTime](time/LocalDateTime.md) | Date paired with time, no zone |
| [ZonedDateTime](time/ZonedDateTime.md) | Date-time with a fixed UTC offset |
| [ZoneId](time/ZoneId.md) | Region time-zone identifier |
| [ZoneOffset](time/ZoneOffset.md) | Fixed offset from UTC |
| [DateTimeFormatter](time/DateTimeFormatter.md) | Temporal value ⇄ text formatting |

## cajeta.io

| Class | |
|---|---|
| [Buffer](io/Buffer.md) | Byte buffer with typed, reinterpreting access |

## cajeta.io.file

| Class | |
|---|---|
| [File](io/file/File.md) | Filesystem access: whole-file statics + handle instance |
| [FileInfo](io/file/FileInfo.md) | One-`stat` snapshot: size, timestamps, type, permissions |
| [FileReader](io/file/FileReader.md) | Streaming byte reader |
| [FileWriter](io/file/FileWriter.md) | Streaming byte writer |
| [Path](io/file/Path.md) | Immutable filesystem path |
| [Watcher](io/file/Watcher.md) | Filesystem-change notifier |

## cajeta.io.net

| Class | |
|---|---|
| [IpAddress](io/net/IpAddress.md) | Immutable IPv4/IPv6 address |
| [SocketAddress](io/net/SocketAddress.md) | IP address + port |
| [TcpStream](io/net/TcpStream.md) | Connected blocking TCP stream |
| [TcpListener](io/net/TcpListener.md) | Passive TCP listening socket |
| [UdpSocket](io/net/UdpSocket.md) | UDP datagram socket |
| [Server](io/net/Server.md) | TCP server core: bind, listen, accept loop |
| [ServerBuilder](io/net/ServerBuilder.md) | Model-selection builder for `Server` |

## cajeta.io.net.dns

| Class | |
|---|---|
| [Dns](io/net/dns/Dns.md) | Blocking hostname resolution |

## cajeta.io.net.uri

| Class | |
|---|---|
| [Uri](io/net/uri/Uri.md) | Immutable parsed URI (RFC 3986) |
| [UriBuilder](io/net/uri/UriBuilder.md) | Builder for `Uri` |

## cajeta.io.net.http

| Class | |
|---|---|
| [HttpClient](io/net/http/HttpClient.md) | HTTP/1.1 client |
| [HttpServer](io/net/http/HttpServer.md) | HTTP/1.1 server |
| [HttpServerBuilder](io/net/http/HttpServerBuilder.md) | Fluent builder for `HttpServer` |
| [Router](io/net/http/Router.md) | Minimal HTTP router |

## cajeta.io.net.tls

| Class | |
|---|---|
| [TlsConnection](io/net/tls/TlsConnection.md) | One TLS connection's engine state |
| [TlsListener](io/net/tls/TlsListener.md) | Server-side TLS termination over `TcpListener` |

## cajeta.io.net.ws

| Class | |
|---|---|
| [WebSocket](io/net/ws/WebSocket.md) | Live RFC 6455 WebSocket connection |

## cajeta.hash

| Class | |
|---|---|
| [Hash](hash/Hash.md) | Hash utility namespace |
| [DefaultHasher](hash/DefaultHasher.md) | Process-seeded XXH3-64 behind synthesized `hash()` |
| [XXHash3](hash/XXHash3.md) | Fast 64-bit non-cryptographic hash |
| [SipHash](hash/SipHash.md) | DoS-resistant keyed 64-bit hash |
| [Blake3](hash/Blake3.md) | Fast modern cryptographic hash (and XOF) |
| [Sha256](hash/Sha256.md) | SHA-256 digest |
| [Sha1](hash/Sha1.md) | SHA-1 digest |
| [MD5](hash/MD5.md) | MD5 checksum / identifier |

## cajeta.math

| Class | |
|---|---|
| [Tensor](math/Tensor.md) | The keystone n-dimensional array |
| [DType](math/DType.md) | Runtime numeric dtype descriptor |
| [Rotation](math/Rotation.md) | Quaternion construction ergonomics |
| [Transform](math/Transform.md) | TRS rigid+scale transform |
| [Camera](math/Camera.md) | Projection and view matrix builders |
| [Ray](math/Ray.md) | Half-line query primitive for picking and culling |
| [Color](math/Color.md) | RGBA color, sRGB ⇄ linear |

## cajeta.math.* (numerics)

| Class | |
|---|---|
| [Fft](math/fft/Fft.md) | Discrete Fourier transform surface |
| [LinAlg](math/linalg/LinAlg.md) | Linear-algebra factorizations |
| [Generator](math/random/Generator.md) | Random numbers over Philox4x32-10 |
| [Stats](math/stats/Stats.md) | Descriptive statistics: histogram, bincount, digitize |
| [Poly](math/poly/Poly.md) | Polynomial operations |
| [Npy](math/npio/Npy.md) | NumPy `.npy` binary array I/O |

## cajeta.codec

| Class | |
|---|---|
| [Base64](codec/Base64.md) | Base64 (RFC 4648) byte ⇄ text codec |
| [Csv](codec/csv/Csv.md) | Typed CSV: bind rows to a declared type |
| [Json](codec/json/Json.md) | Top-level JSON parse/serialize entry points |

## cajeta.wire

| Class | |
|---|---|
| [Encoder](wire/Encoder.md) | Bidirectional wire codec: `T` ⇄ bytes |
| [Schema](wire/Schema.md) | The schema a `SchemaEncoder` decodes against |
| [SchemaEncoder](wire/SchemaEncoder.md) | Schema-carrying wire codec |
| [Compressor](wire/Compressor.md) | Block-compress stage |
| [Decompressor](wire/Decompressor.md) | Block-decompress stage |

## cajeta.search

| Class | |
|---|---|
| [Distance](search/distance/Distance.md) | String edit-distance functions |
| [Index](search/ngram/Index.md) | N-gram inverted index for fuzzy candidates |
| [Matcher](search/fuzzy/Matcher.md) | Typo-tolerant key→value lookup |

## cajeta.process

| Class | |
|---|---|
| [Command](process/Command.md) | Subprocess command: program, args, env, capture |
| [Process](process/Process.md) | A spawned, still-running child |

## cajeta.error

| Class | |
|---|---|
| [Throwable](error/Throwable.md) | Root of the exception hierarchy: message, diagnostic code, stack trace, JSON |
| [Exception](error/Exception.md) | Base of the catchable exception hierarchy |
| [RecoverableException](error/RecoverableException.md) | Errors a caller might handle and proceed past |
| [NoOptionalValueException](error/NoOptionalValueException.md) | Unwrap of an empty `Optional` — catchable, not panic |
| [UnrecoverableException](error/UnrecoverableException.md) | Fatal conditions: invariant violations, OOM |

## cajeta.reflect

| Class | |
|---|---|
| [Class](reflect/Class.md) | Runtime class representation; the reflection entry point |

## cajeta.xpu

| Class | |
|---|---|
| [Device](xpu/Device.md) | The active XPU device and host-side queries |
| [KernelBuffer](xpu/KernelBuffer.md) | Unified handle to device memory |
| [KernelStream](xpu/KernelStream.md) | Ordered queue of XPU work |
| [MeshSimplifier](xpu/mesh/MeshSimplifier.md) | Garland–Heckbert edge-collapse mesh simplifier |

## cajeta.gfx

| Class | |
|---|---|
| [Texture2D](gfx/Texture2D.md) | Read-only 2-D float32 texture, sampled in kernels |
| [Sampler](gfx/Sampler.md) | Filtering + addressing configuration for `Texture2D` |

## cajeta.ifx

| Class | |
|---|---|
| [BackendRegistry](ifx/BackendRegistry.md) | UI backend registry, probe, and dispatcher |
| [Window](ifx/Window.md) | The portable window contract |

Coverage is checked: `scripts/check-stdlib-coverage.sh` fails if any class
with an `@EntryPoint`-tagged member lacks a document here.
