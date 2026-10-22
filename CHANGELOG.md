# Changelog

## 0.46.1 (2026-10-22)

- **A dependency's skills are found.** `cajeta build` now writes `cajeta.lock` with every resolved package, as the build-tool spec says it does; before, no command wrote packages into it, so `search-skill`, `list-skills`, `get-skills` and the MCP server saw only the stdlib's skills and never a dependency's. `cajeta info --write-lockfile` records the packages too. An unchanged graph leaves the file byte-identical, and a workspace member's build does not write one. A build that would leave skills unpacked now warns and names them: a `skills/` directory inside the source tree, or the project's `skills/` when the bare compiler is given only a source root (pass `--skill-root=<project root>`, which `cajeta build` does). dev.cajeta.codec 0.9.0, crypto 0.1.0 and cloud 0.4.0 all shipped without their skills this way.

## 0.46.0 (2026-10-21)

- **`CooperativeMatrix.addScaledPow2(src, e)`** adds `src * 2^e` to a float32 accumulator tile of the same shape, for a constant `e` in [-126, 127]. The product is exact, so every backend and the reference interpreter agree bit for bit. It is not an epilogue verb, so a Vulkan kernel that uses it keeps its native cooperative-matrix tiles instead of falling back to the software tile; on Vulkan it lowers to `OpMatrixTimesScalar` and `OpFAdd`. A wrong shape, a non-accumulator tile, int32 elements, or a non-constant or out-of-range exponent is a named diagnostic.
- The Vulkan device enables `VK_KHR_cooperative_matrix` and its feature, and creates cooperative-matrix pipelines with full subgroups at the declared wave width. It declared cooperative matrices and requested full subgroups without enabling either; RADV ran the shaders anyway, and the validation layer reported them.
- The toolchain builds against the cajeta LLVM fork's `cajeta-llvm-23-r11`, which adds the two SPIR-V cooperative-matrix intrinsics `addScaledPow2` lowers to.
- **`TileOps.scaledAccumI32` takes both factors as their low 24 bits**, sign-extended, with the sum wrapping at 32 bits, which is the contract of the built-in it replaces. On AMD that is the full-rate 24-bit multiply-add (`v_mad_i32_i24`) where it was a quarter-rate 64-bit one: cajeta-llm's quantized prefill tiles went from 244 to 212 VGPRs and its 8B prefill on gfx1151 from 1134 to 1237 tok/s. A caller passing factors wider than 24 bits now gets the built-in's answer rather than the full product.
- **The build cache sees native archives.** The whole-artifact cache key (`.cajeta/cache/artifact`) folds every native file a build could bake or link: a shim provided after a cached build, rebuilt from edited C, removed, or added for another platform now re-keys the artifact, where 0.45.0 re-published the stale `.cja` until `.cajeta/cache` was cleared. The project's own `native/` directory is now searched for `@Native` archives ahead of `CAJETA_NATIVE_PATH` and `~/.cajeta/native`: the build tool passes it to the compiler as `--native-dir`, so a library checkout bakes its shim with no environment variable set.

## 0.45.0 (2026-10-20)

- **Dependencies are verified by default** (publisher-trust spec 4.1, 5.3 to 5.5). Every repository a manifest declares now checks an archive before it is stored or loaded: `cajeta build`, `deps`, plugins, melts and a notebook's `Packages.install` run the same check. The repository must serve release metadata signed through its root (olla's ships in the toolchain), the bytes must hash to it, and when that metadata names an organization the archive must carry a signature that verifies against the organization's key document. A repository that signs nothing, such as a local or vendored one, is refused until its `settings.repositories` entry says `"verification": "checksum"`; that relaxes that repository only, and a melt cannot relax the repositories it contributes. A present envelope that does not verify is refused under either policy. `~/.olla` keeps the envelope beside each mirrored archive, and the machine's own `cajeta install` builds still resolve. `settings.require-signatures` is retired: it is still read, has no effect, and warns.

  Migration: add `"verification": "checksum"` to any filesystem, git or private HTTP repository that does not sign its releases. Olla's 49 releases published before it signed metadata (2026-07-19 to 2026-09-03, all superseded) are admitted on their exact bytes, which this toolchain lists by sha256.
- The client read olla's capability document wrong: olla nests its flags under `"capabilities"` with `"ttl-seconds"`, and the client looked only for `"protocol-versions"`, so it never saw v2 on olla and never read a signed release envelope. Both forms are read now.
- **A lambda's owned locals are dropped.** A lambda body had no drop frames, so every owned local in it leaked, the drop chain's head was left in the dead lambda frame, and a loop-body local re-pushed its own slot into a self-loop. A throw that later unwound past it spun forever at 100% of one core. Libraries ship precompiled bitcode, so their lambdas pick this up when rebuilt on 0.45.0.
- **A local changed inside a `try` keeps its value after a throw** at O1 and above, `--release` included. `try` is setjmp/longjmp and the optimizer kept such locals in registers, so the catch and the code after it read the value from `try` entry; 0.43.1 and 0.44.0 did the same. Each local written in the try body and read after the second return now stays in memory.
- `--release` executables run element access several times faster: line frames are pushed and popped inline and coalesced into one stack-top update per run, small methods inline across modules without LTO, a virtual call whose every vtable entry is one function is called directly, and a method that only throws no longer allocates a scope frame. `CAJETA_INLINE_LINE_FRAMES=0`, `CAJETA_LINE_FRAME_COALESCE=0`, `CAJETA_XMODULE_INLINE=0` and `CAJETA_DEVIRT=0` turn each off. ElementAccessBench `flatGet` went from about 14 ns to 1 ns per element.
- `Npy` encodes and decodes through the host buffer: decoding 2^23 int32 values went from 123 ms to 7 ms.
- xpu: a kernel whose workgroup memory exceeds the device's limit is unavailable (`Device.kernelAvailable` answers false) and a launch is refused by name before the driver, reason 7 in `Device.checkLaunch`. On Vulkan such a kernel reached the driver and RADV divided by zero.
- Windows: `Path.mkdirs` treats a drive prefix as a root, so a nested directory on another drive is created.

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
