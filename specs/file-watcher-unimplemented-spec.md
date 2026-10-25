# file-watcher-unimplemented — `cajeta.io.file.Watcher` is a documented placeholder (defect spec)

> Status: **draft**, filed 2026-10-25 by benchmark-reference unit 10 on
> v0.46.3.

## 1. Definition
`docs/stdlib/io/file/Watcher.md` documents filesystem change notification,
and the class exists, but it has no watch call and no event stream: nothing
can be observed through it. `io/watcher-event-latency` skips on the Cajeta
side with that reason while every competitor runs. Related gaps from the same
unit, which the io pages now state: `FileWriter` is unbuffered, `FileReader`
has no line API, `File` has no positional read, `FileInfo` has no stat
accessor, `Path` has no `normalize`, there is no directory walk, `Buffer` has
no store, slice or typed loads, and `ByteBuffer` has no typed big-endian
accessors although the io README says it does.

## 2. Use cases
- **2.1** As a program, when I watch a directory, then I receive create,
  modify and delete events (inotify on Linux, ReadDirectoryChangesW on
  Windows, FSEvents or kqueue on macOS) through an async stream.
- **2.2** As a reader of the stdlib reference, then no page documents a
  class that cannot do what the page says; until the watcher ships, its page
  says so.

## 3. Acceptance
`io/watcher-event-latency` measures Cajeta; the io README's ByteBuffer claim
is true or removed.
