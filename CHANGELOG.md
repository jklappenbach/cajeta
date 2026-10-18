# Changelog

## 0.44.0 (2026-10-18)

- **Cryptography leaves the stdlib for the `dev.cajeta.crypto` library** (cajeta-crypto spec 6.3; [cajeta-crypto](https://github.com/jklappenbach/cajeta-crypto) 0.1.0). Each type keeps its name and API; add `"dev.cajeta.crypto": "0.1.0"` to `dependencies` and change the import:

  | Removed | New import |
  |---|---|
  | `cajeta.hash.Sha256` | `dev.cajeta.crypto.Sha256` |
  | `cajeta.hash.Sha1` | `dev.cajeta.crypto.Sha1` |
  | `cajeta.hash.MD5` | `dev.cajeta.crypto.MD5` |
  | `cajeta.hash.Blake3` | `dev.cajeta.crypto.Blake3` |
  | `cajeta.hash.HmacSha256` | `dev.cajeta.crypto.HmacSha256` |
  | `cajeta.hash.HmacSha1` | `dev.cajeta.crypto.HmacSha1` |
  | `cajeta.hash.Pbkdf2` | `dev.cajeta.crypto.Pbkdf2` |
  | `cajeta.hash.Totp` | `dev.cajeta.crypto.Totp` |
  | `cajeta.hash.SecureRandom` | `dev.cajeta.crypto.SecureRandom` |
  | `cajeta.io.net.tls.TlsStream` | `dev.cajeta.crypto.tls.TlsStream` |
  | `cajeta.io.net.tls.TlsListener` | `dev.cajeta.crypto.tls.TlsListener` |
  | `cajeta.io.net.tls.TlsConnection` | `dev.cajeta.crypto.tls.TlsConnection` |
  | `cajeta.io.net.tls.TlsException` | `dev.cajeta.crypto.tls.TlsException` |
  | `cajeta.io.net.tls.CertificateInvalidException` | `dev.cajeta.crypto.tls.CertificateInvalidException` |

  Code that still uses a removed type fails with `CAJETA_ERROR_UNRESOLVED_TYPE` naming it. `cajeta.hash.Hasher`, `XXHash3`, `SipHash`, `DefaultHasher` and `Hash.constantTimeEquals` stay, and so does `cajeta.codec.Base32`. The digests' runtime C, `__cajeta_secure_random_fill`, the TLS engine and the TLS object the compiler embedded for `--emit=exe` are gone; an exe still links `-lssl -lcrypto` (and `-lcrypt32` on Windows) for the library's shim, and the driver still exports OpenSSL to its JIT (`scripts/check-jit-openssl.sh`).
- xpu: a launch whose bindless buffer array holds more than 16 buffers is refused and counted on every backend. The launch site clamped the count to 16 before storing it, so a 17-buffer launch ran silently with 16; cuda and hip now refuse it by count, cpu checks each array against the cap before running (it used to read `bufs[16]` off its slot), and vulkan refuses it as before. `Device.launchFailures()` also counts cuda's texture-allocation, shared-memory opt-in and argument-translation exits and hip's launch exit, which returned without counting.
- Windows: the static release `cajeta.exe` binds winpthread, the 64-bit time calls and emutls in its JIT symbol bridge, so `cajeta jit-run` and `cajeta test` run programs again; v0.43.1's could run none ("Symbols not found: pthread_create, ..."). The release workflow smoke-tests `jit-run`.
- Windows: under Git Bash or MSYS2, `--classpath` entries spelled `/d/...` resolve to `D:/...`, and `--emit=exe` falls back to MSYS2's `C:/msys64/mingw64/bin` gcc or clang when no C driver is on `PATH`.

## 0.43.1 (2026-10-17)

- Runtime: an idle carrier sleeps when the only runnable fiber is pinned to a busy peer. A started fiber resumes only on its home carrier, and every other carrier that found nothing it could take went round again at full CPU for the whole of that carrier's turn: three threads at 100% beside one hashing a password, and on a two-vCPU CI runner the spinner shared a physical core with the worker, so primavera's test step went from 18 minutes to past its 30-minute cap when its server moved to cajeta-http 0.7.0's pool. A carrier now looks twice more, then sleeps on its 50 ms backstop as for an empty pool. Measured on the register-user sample under two CPUs: 15:18 before, 10:17 after.
- xpu: a launch whose block exceeds the device's work-group limit is refused before submit (`XpuLaunchException`, reason 6) instead of reaching the driver; on gfx1151 it hung the GPU. cuda and hip now count a launch the driver rejects.

## 0.43.0 (2026-10-16)

- `cajeta.io.net.reactor.ReadySet`: one fiber waits on many descriptors at once. Members persist across waits, a wait returns the members that became ready, and a fired member is re-armed by the next wait, so a wait costs the ready set and never the membership. On Linux it rides the epoll fiber-park engine; elsewhere it probes its members with a cooperative sleep until the kqueue and IOCP engines land. The framers of request dispatch wait on it (request-dispatch spec §3.4).
- `cajeta.concurrent.Carriers.count()`: how many carrier threads run fibers, `CAJETA_CARRIERS` when set and otherwise the smaller of the core count and the runtime's cap, resolved once for the process. A server sizes its framers by it.
- `TcpStream.readNow`: a read that answers `-1` when the socket would block instead of parking, the count when bytes are there, and `0` at EOF.
- `Channel.trySend`: a send that answers `false` on a full or closed channel instead of parking.
- The reactor's closing path reports a closed descriptor to the set that held it, and `CAJETA_REACTOR_TRACE=1` traces set arms, records, drops and re-arms beside the single-descriptor waits.
- `TlsStream` gains the entries a framer drives it with, none of them parking: `fd`, `fillNow`, `handshakeNow`, `flushNow` (a partial write stays pending for the next call), `readPlaintextNow` straight into a `ByteBuffer`, `writeNow` from an offset, `pendingOut`.
- The `TcpStream.read` and `write` intrinsics load a field array argument (`this.outBuf`) as the array rather than its slot's address. The bytes on the wire were the right length and the wrong contents whenever a buffer was a field instead of a local.
- A wire view built over a field array, `Header(this.buf)`, reads the array. The view constructor loaded only a local's slot; a field or element slot was handed to the view as the bytes, so the view read the slot's pointer as its first fields. Both spellings now agree (`MultiVarSizeViewTests.viewOverAFieldArrayReadsTheArray`).
- Runtime: the fiber pool's condvars (carrier backstop, timer thread, the program thread's timed wait) wait on CLOCK_MONOTONIC on Linux, so a wall-clock step no longer moves a timed park (991f5a55).
- Windows: the JIT symbol bridge binds `TlsStream`'s new non-parking entries, so a JIT-run program or test that links the stdlib loads on COFF; `scripts/check-jit-win-symbols.sh` keeps every TLS entry point bound.

## 0.42.1 (2026-10-15)

- A multibinding container (`@Inject ArrayList<T>`, `@Inject HashMap<String, T>`), a `@<Ctor>(staticName)` factory's instance and a constructor reference's instance (`T::heap`) are zero-filled before their constructor runs, like every `heap` instance. They were built on bare malloc bytes, so a constructor's `#=` field store read the field's ownership bit from heap garbage and could drop a garbage pointer; primavera's register-user sample faulted at startup in `ArrayList`'s constructor on v0.42.0. A library whose archive was built by an earlier compiler carries the old code until it is rebuilt.
- The JIT test harness takes the fresh per-test path on Windows: its shared stdlib dylib crashed 114 of 901 release tests on COFF.

## 0.42.0 (2026-10-14)

- A String temporary passed to a borrow formal (`raise("failed the " + check + " check")`) is reclaimed when the callee throws. It was dropped after the call, which a throw skipped: 2 live objects per throw, hit by cajeta-cloud's `badToken` on every refused token. The temporary now holds a drop-chain entry around the call, so the throw's unwind frees it.
- Two archives that declare the same short type names in different packages (primavera's and cajeta-cloud's `TimeSource` and `ManualTimeSource`) load together. A class read before its interface used to bind to the other archive's interface (`CAJETA_ERROR_INTERFACE_NOT_IMPLEMENTED`), and `heap a.b.C()` built a same-named class from another package and faulted. Declared parents resolve in their own package first; a qualified creator builds the class it names.
- `XXHash3.hash` over inputs longer than 240 bytes no longer traps: its long path ran signed `int64` arithmetic, which traps on overflow, so every long hash ended in SIGILL. The scalar steps now run in `uint64`, as XXH3 means them.
- xpu: every launch is a submission. `k.launch(s, grid:, block:)(args)` reaches the runtime as a default-policy submission; `KernelSubmission` gains `earliestNs`, `latestNs`, `protectedLaunch`, `cuBudget`, `captureId`, `energyJoules`, `poolHandle`, `poolReserve` and `stream`; `SchedulePolicy` adds `Energy`. A submission whose window closed is rejected before dispatch with `XpuLaunchException`. `Scheduler.bypass(true)` or `CAJETA_XPU_SCHEDULER=bypass` takes the raw launch path. `cajeta profile summary` lists instrumentation records under the kernels.
- xpu: device calibration. `cajeta.xpu.calibration.DeviceCalibration` measures a device's peaks, bandwidth, launch cost and scheduling delay on first contact and keeps them through a `CalibrationStore` (records embedded at build, then the platform cache). `cajeta calibrate <program>` records a program's launches and replays them on this device into a snapshot; `cajeta build --with-calibration=<snapshot>` embeds the snapshot's records. The package is lazy, so programs that do not use it compile no calibration kernels.
- `@GenerateMock` on an interface: the mock implements it and forwards its own and inherited methods; a class target's whole chain is forwarded; `#R` methods return the engine's owned answer. The generated mock is a `@TestComponent`, injected in place of the production component under the test profile.
- xpubench: arms interleave in one process in Thue-Morse order, profiled passes are rounds with their own walls, and a contention witness marks KPIs measured beside foreign CPU or GPU load as `contended` rather than `worse`.
- The test harness JITs the stdlib once per process and links each test against it, so a test's JIT cost drops from about 11 s to about 50 ms and a full sweep's recorded test time from 31 to 19 CPU-hours. The runtime gains execution epochs (`__cajeta_rt_epoch_push` and `__cajeta_rt_epoch_pop_to`), which a host running several programs against one runtime uses to drop what a program's constructors registered. The stdlib's machine code is kept across test processes under `.cajeta/cache/prime/obj`, so a warm process links it in about 1.4 s instead of compiling it for about 10 s. The sweep primes that cache once before its shards start, so a cold sweep pays one stdlib compile instead of one per shard. Each sweep shard then runs one fork server that primes once and forks a child per chunk (`FORK=0` restores a process per chunk), 14.2 CPU-hours of recorded test time against 16.6.

## 0.41.0 (2026-10-13)

- An interface value held as an interface it extends dispatches that interface's methods. It used to call the derived interface's slots (silent wrong code), and with the base in a later file it faulted. Each (class, interface) table now ends with pointers to the class's tables for the interfaces it extends, so archives built by an older compiler must be rebuilt.
- A cast to an interface that extends the value's own (`(Wide) b`) finds the class's table at run time and throws `ClassCastException` when the class does not implement it. Interface casts now carry ownership like class casts. Each table gains a trailing directory slot.
- Integer widening extends by the source's signedness: a `uint32` above 2^31 now widens to the same positive value instead of a negative one.
- A templated interface used before the class it is instantiated over has been read (`Codec<Signup>` in a signature, `Signup` in a later file) keeps its methods. Calls through it were `CAJETA_ERROR_MEMBER_NOT_FOUND`; the stdlib's `Encoder<T>` was affected the same way.
- A lambda that captures an interface value keeps its own copy of it, so it no longer reads a dead frame once the function that converted the value returns.
- Templates: a bound holds through `implements` and waits for a type not yet parsed; a class template's bare name inside its own body names the instantiation.
- xpu: kernel manifests carry a work model derived from the kernel body, and manifest bytes are the bytes a kernel requests. `Scheduler.submit` reads a kernel's footprint from its manifest. API change: `KernelResourceDescriptor`'s constructor drops `vgprCount` and `ldsBytes`, and `KernelSubmission.ldsBytes` is renamed `sharedBytes`. A kernel declaring `@Cost` fails the build.
- xpu: `KernelManifest.of` returns a lent view of a record parsed once per process, so a program may keep the record and a submit no longer pays the JSON parse. Its signature changes from `#KernelManifest` to `KernelManifest`; a caller that binds the result with `#=` keeps compiling, since `#=` records the borrow.
- xpu: `Bits` over `uint64`, with `trailingZeros` and `leadingZeros` at both widths. A Vulkan launch of a kernel with no Vulkan device code is refused, as on the other backends.

## 0.40.0 (2026-10-11)

- `int64` wave verbs: `Wave.reduceSumI64`, `reduceMaxI64`, `reduceMinI64`, `prefixSumI64` and the four `shuffle*SyncI64` forms.
- `Workgroup.reduce` takes `int64`; new `Workgroup.scan`, an exclusive scan over `int32` and `int64`.
- cpu backend: a local assigned under a per-lane branch and read after a barrier is now per work-item (it read the last work-item's value).
- cpu backend: barrier fission fixes for `break` out of a uniform loop, zero-initialized uniform slots, and the global index over a pinned block.
- `@Unlowered(hold = true)` skips a kernel on every backend; a class template keeps an interface declared in a later file.
