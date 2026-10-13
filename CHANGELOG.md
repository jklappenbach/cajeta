# Changelog

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
