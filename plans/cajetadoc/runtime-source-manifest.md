# Cajeta runtime source manifest

Every `.cajeta` source file under `runtime/`, **excluding** `cajeta.xpu` (GPU)
files. This is the work-list for the cajetadoc documentation pass — 110 files.

Generated: 2026-06-05.

> **DONE.** The cajetadoc documentation pass this work-list drove has landed (commit
> `50da718` + follow-ups); all non-xpu `runtime/` sources now carry `/**` doc comments.
> This is a regenerable static manifest — its purpose is fulfilled; the 110 count predates
> later source additions.

## cajeta.codec.json
- runtime/src/cajeta/codec/json/Json.cajeta
- runtime/src/cajeta/codec/json/JsonArray.cajeta
- runtime/src/cajeta/codec/json/JsonObject.cajeta
- runtime/src/cajeta/codec/json/JsonParseException.cajeta
- runtime/src/cajeta/codec/json/JsonReader.cajeta
- runtime/src/cajeta/codec/json/JsonToken.cajeta
- runtime/src/cajeta/codec/json/JsonValue.cajeta
- runtime/src/cajeta/codec/json/JsonWriter.cajeta

## cajeta.collection
- runtime/src/cajeta/collection/ArrayList.cajeta
- runtime/src/cajeta/collection/BPlusTree.cajeta
- runtime/src/cajeta/collection/BPlusTreeNode.cajeta
- runtime/src/cajeta/collection/Cache.cajeta
- runtime/src/cajeta/collection/CacheNode.cajeta
- runtime/src/cajeta/collection/Collector.cajeta
- runtime/src/cajeta/collection/Collectors.cajeta
- runtime/src/cajeta/collection/HashMap.cajeta
- runtime/src/cajeta/collection/HashSet.cajeta
- runtime/src/cajeta/collection/Heap.cajeta
- runtime/src/cajeta/collection/ImmutableList.cajeta
- runtime/src/cajeta/collection/ImmutableMap.cajeta
- runtime/src/cajeta/collection/ImmutableSet.cajeta
- runtime/src/cajeta/collection/LinkedList.cajeta
- runtime/src/cajeta/collection/LinkedListNode.cajeta
- runtime/src/cajeta/collection/RedBlackNode.cajeta
- runtime/src/cajeta/collection/RedBlackTree.cajeta
- runtime/src/cajeta/collection/ltm/LtmBPlusTree.cajeta
- runtime/src/cajeta/collection/ltm/LtmBPlusTreeNode.cajeta
- runtime/src/cajeta/collection/ltm/LtmPager.cajeta

## cajeta.error
- runtime/src/cajeta/error/Exception.cajeta
- runtime/src/cajeta/error/RecoverableException.cajeta
- runtime/src/cajeta/error/Throwable.cajeta
- runtime/src/cajeta/error/UnrecoverableException.cajeta

## cajeta.hash
- runtime/src/cajeta/hash/DefaultHasher.cajeta
- runtime/src/cajeta/hash/Hash.cajeta
- runtime/src/cajeta/hash/Hasher.cajeta
- runtime/src/cajeta/hash/MD5.cajeta
- runtime/src/cajeta/hash/SipHash.cajeta
- runtime/src/cajeta/hash/XXHash3.cajeta

## cajeta.io.file
- runtime/src/cajeta/io/file/AlreadyExistsException.cajeta
- runtime/src/cajeta/io/file/CrossDeviceException.cajeta
- runtime/src/cajeta/io/file/DiskFullException.cajeta
- runtime/src/cajeta/io/file/EndOfFileException.cajeta
- runtime/src/cajeta/io/file/File.cajeta
- runtime/src/cajeta/io/file/FileEvent.cajeta
- runtime/src/cajeta/io/file/FileInfo.cajeta
- runtime/src/cajeta/io/file/FileReader.cajeta
- runtime/src/cajeta/io/file/FileWriter.cajeta
- runtime/src/cajeta/io/file/IoException.cajeta
- runtime/src/cajeta/io/file/IsDirectoryException.cajeta
- runtime/src/cajeta/io/file/NotDirectoryException.cajeta
- runtime/src/cajeta/io/file/NotFoundException.cajeta
- runtime/src/cajeta/io/file/OpenMode.cajeta
- runtime/src/cajeta/io/file/Path.cajeta
- runtime/src/cajeta/io/file/PermissionException.cajeta
- runtime/src/cajeta/io/file/Watcher.cajeta
- runtime/src/cajeta/io/file/WatchKind.cajeta

## cajeta.lang
- runtime/src/cajeta/lang/Comparable.cajeta
- runtime/src/cajeta/lang/Encoding.cajeta
- runtime/src/cajeta/lang/EncodingErrorPolicy.cajeta
- runtime/src/cajeta/lang/EncodingException.cajeta
- runtime/src/cajeta/lang/Math.cajeta
- runtime/src/cajeta/lang/Object.cajeta
- runtime/src/cajeta/lang/Optional.cajeta
- runtime/src/cajeta/lang/Pair.cajeta
- runtime/src/cajeta/lang/String.cajeta

## cajeta.lang.stream
- runtime/src/cajeta/lang/stream/ArrayStream.cajeta
- runtime/src/cajeta/lang/stream/FilterStream.cajeta
- runtime/src/cajeta/lang/stream/FlatMapStream.cajeta
- runtime/src/cajeta/lang/stream/HashMapEntryStream.cajeta
- runtime/src/cajeta/lang/stream/HashMapKeyStream.cajeta
- runtime/src/cajeta/lang/stream/HashMapValueStream.cajeta
- runtime/src/cajeta/lang/stream/MapOrFallbackStream.cajeta
- runtime/src/cajeta/lang/stream/MapOrLogStream.cajeta
- runtime/src/cajeta/lang/stream/MapOrSkipStream.cajeta
- runtime/src/cajeta/lang/stream/MapStream.cajeta
- runtime/src/cajeta/lang/stream/ParallelDriver.cajeta
- runtime/src/cajeta/lang/stream/PeekStream.cajeta
- runtime/src/cajeta/lang/stream/SkipStream.cajeta
- runtime/src/cajeta/lang/stream/Splittable.cajeta
- runtime/src/cajeta/lang/stream/Stream.cajeta
- runtime/src/cajeta/lang/stream/TakeStream.cajeta

## cajeta.threading
- runtime/src/cajeta/threading/AsyncIterator.cajeta
- runtime/src/cajeta/threading/AtomicInt32.cajeta
- runtime/src/cajeta/threading/AtomicInt64.cajeta
- runtime/src/cajeta/threading/Channel.cajeta
- runtime/src/cajeta/threading/Lock.cajeta
- runtime/src/cajeta/threading/LockGuard.cajeta
- runtime/src/cajeta/threading/Mutex.cajeta
- runtime/src/cajeta/threading/RwLock.cajeta
- runtime/src/cajeta/threading/SelectResult.cajeta
- runtime/src/cajeta/threading/Semaphore.cajeta
- runtime/src/cajeta/threading/Tasks.cajeta
- runtime/src/cajeta/threading/WriteGuard.cajeta

## cajeta.time
- runtime/src/cajeta/time/Clock.cajeta
- runtime/src/cajeta/time/DateTimeException.cajeta
- runtime/src/cajeta/time/DateTimeFields.cajeta
- runtime/src/cajeta/time/DateTimeFormatter.cajeta
- runtime/src/cajeta/time/DateTimeParseException.cajeta
- runtime/src/cajeta/time/Duration.cajeta
- runtime/src/cajeta/time/Fmt.cajeta
- runtime/src/cajeta/time/FormatStyle.cajeta
- runtime/src/cajeta/time/Instant.cajeta
- runtime/src/cajeta/time/LocalDate.cajeta
- runtime/src/cajeta/time/LocalDateTime.cajeta
- runtime/src/cajeta/time/LocalTime.cajeta
- runtime/src/cajeta/time/Period.cajeta
- runtime/src/cajeta/time/ZoneId.cajeta
- runtime/src/cajeta/time/ZoneOffset.cajeta
- runtime/src/cajeta/time/ZonedDateTime.cajeta

## cajeta.wire
- runtime/src/cajeta/wire/Encoder.cajeta
