# Specification index

The design and specification documents behind every Cajeta subsystem. Each
group leads with its overview document. The [guide](../guide/README.md) teaches
the language; the [stdlib reference](../stdlib/README.md) documents the API
surface; these documents record the design decisions underneath both.

## The language — `lang/`

Core semantics: types, ownership, memory, dispatch.

- [MemoryModel](lang/MemoryModel.md) — stack/heap allocation, single ownership, borrows, drops.
- [OwnershipTransfer](lang/OwnershipTransfer.md) — the `#` operator at call sites.
- [FieldOwnership](lang/FieldOwnership.md) — field ownership and auto-drop.
- [FieldBorrowEscape](lang/FieldBorrowEscape.md) — field-store borrow escape.
- [BorrowSoundness](lang/BorrowSoundness.md) — detection beyond the type system.
- [slice-spec](lang/slice-spec.md) — slices and the `shared` state.
- [ValueReturns](lang/ValueReturns.md) — returning stack-allocated values.
- [Views](lang/Views.md) — layout-pinned views over buffers.
- [UnifiedClasses](lang/UnifiedClasses.md) — unified classes v2.
- [MultiClassing](lang/MultiClassing.md) — collision handling and selection.
- [Lambdas](lang/Lambdas.md) — lambdas and method references.
- [OperatorOverloading](lang/OperatorOverloading.md) — operator methods.
- [AspectModel](lang/AspectModel.md) — AOP + dependency injection in the core language.
- [DI-override-hook](lang/DI-override-hook.md) — test-only `@Inject` substitution.
- [Primitives](lang/Primitives.md) — unboxed types and their boxed wrappers.
- [FloatingPointModel](lang/FloatingPointModel.md) — sub-byte and 8-bit float support.
- [EncodingPrefixedLiterals](lang/EncodingPrefixedLiterals.md) — encoding-prefixed byte-array literals.
- [LintRules](lang/LintRules.md) — the lint catalog.

### Templates — `lang/templates/`

- [TemplateWildcard](lang/templates/TemplateWildcard.md) — wildcards (`<?>`, PECS, capture).
- [MethodLevelTemplate](lang/templates/MethodLevelTemplate.md) — per-method type parameters.
- [NumericBoundedTemplates](lang/templates/NumericBoundedTemplates.md) — `Numeric` / `Integral` / `Floating` bounds (reference).
- [numeric-bounds-spec](lang/templates/numeric-bounds-spec.md) — the numeric marker hierarchy (spec).
- [reified-capture-spec](lang/templates/reified-capture-spec.md) — recovering concrete types from wildcards.

### `cajeta.lang` — root types and streams

- [cajeta.lang](lang/Lang.md) — the root types.
- [Object](lang/Object.md) — the universal root.
- [String](lang/String.md) — immutable UTF-8 text.
- [System](lang/System.md) — process-level intrinsics.
- [Locale](lang/Locale.md) — language/region tags.
- [cajeta.lang.stream](lang/stream/Streams.md) — the stream pipeline model.
- [StreamParallelism](lang/stream/StreamParallelism.md) — parallel stream design, with
  [error handling](lang/stream/StreamParallelism.ErrorHandling.md) and
  [examples](lang/stream/StreamParallelism.Examples.md).

## Standard library

- [cajeta.collection](collection/Collections.md) — lists, sets, maps.
- [cajeta.concurrent](concurrent/Concurrency.md) — fibers, channels, the async runtime;
  [AsyncStatus](concurrent/AsyncStatus.md), [HarnessDesign](concurrent/HarnessDesign.md),
  [FiberLocal](concurrent/FiberLocal.md) (+ [tour](concurrent/FiberLocal-tour.md)).
- [cajeta.error](error/ErrorModel.md) — the error model: throwables, recoverable vs
  unrecoverable; [diagnostic-exceptions-spec](error/diagnostic-exceptions-spec.md).
- [cajeta.io](io/Io.md) — byte substrate and stream abstractions.
  - `cajeta.io.file`: [package overview](io/file/Readme.md), [Path](io/file/Path.md),
    [File](io/file/File.md), [FileReader](io/file/FileReader.md),
    [FileWriter](io/file/FileWriter.md), [OpenMode](io/file/OpenMode.md),
    [FileInfo](io/file/FileInfo.md), [Directories](io/file/Directories.md),
    [Watcher](io/file/Watcher.md), [Errors](io/file/Errors.md).
  - `cajeta.io.net`: [Networking](io/net/Networking.md), [error taxonomy](io/net/Errors.md).
  - [cajeta.io.pipe](io/Pipes.md) — anonymous pipes + FIFOs (design).
- [cajeta.process](process/Process.md) — subprocess control (shipped v1);
  [Process-design](process/Process-design.md) — the fiber-aware direction.
- [cajeta.codec](codec/Codecs.md) — the codec framework;
  [cajeta.codec.json](codec/json/Json.md).
- [cajeta.hash](hash/Hashing.md) — hashing.
- [cajeta.reflect](reflect/Reflection.md) — reflection;
  [Annotations](reflect/Annotations.md).
- [cajeta.time](time/Time.md) — time and durations.

## Math and numerics

- [cajeta.math](math/CajetaMath.md) — the numerical foundation library.
- [cajeta.math.Tensor](math/tensor-spec.md) — the keystone n-d array (spec).
- [numpy-porting-spec](math/numpy-porting-spec.md) — numpy surface classification and placement.
- [cajeta.math.Vector](math/Simd.md) — data-parallel SIMD ops on `Vector<T,N>`
  (+ [tour](math/Simd-tour.md)).
- [cajeta.math.Quaternion](math/Quaternions.md) — quaternion rotation ops.
- [cajeta.math.Matrix](math/MatrixDeterminantInverse.md) — `Matrix<T,R,C>` determinant
  and inverse.

  `Vector`, `Quaternion`, and `Matrix<T,R,C>` are compiler-defined value types, not stdlib
  source; no import needed. They lower to flat LLVM vectors — native SIMD instructions where
  the host CPU has them — and lower the same way inside XPU kernels on the device backends.
- [sorting-spec](sorting/sorting-spec.md) — the comparison seam, host sort, Tensor sort.
- [Núcleo](nucleo/README.md) — the Python scientific-stack port (own index).
- Caramelo (formerly Toffee) — the PyTorch-successor ML framework — is a
  *consumer* of the foundation and lives in its own repo (`cajeta-caramelo`);
  its design spec moved there (`docs/specification/CajetaCaramelo.md`).

## GPU and compute

- [CajetaGPU](gpu/CajetaGPU.md) — the shared device foundation.
- [ValueTypeCatalog](gpu/ValueTypeCatalog.md) — device value types.
- [MaskSelect](gpu/MaskSelect.md), [BitInstructions](gpu/BitInstructions.md),
  [IntegerDotProduct](gpu/IntegerDotProduct.md), [WritableImages](gpu/WritableImages.md) — per-invocation ops.
- [splats](gpu/splats.md) — Gaussian splats.
- [VendorExtensionSDK](gpu/VendorExtensionSDK.md) — vendor extension seed.
- Ray query — `gpu/rayquery/`: [RayQuery](gpu/rayquery/RayQuery.md) (portable BVH noun),
  [native Vulkan](gpu/rayquery/rayquery-native-vulkan-spec.md),
  [OptiX CUDA](gpu/rayquery/rayquery-optix-cuda-spec.md),
  [OptiX M2 codegen](gpu/rayquery/rayquery-optix-m2-codegen-spec.md),
  [OptiX M3 multi-impl](gpu/rayquery/rayquery-optix-m3-multiimpl-spec.md).
- Cooperative matrix — `gpu/coopmatrix/`:
  [NVPTX tensor cores](gpu/coopmatrix/nvptx-tensorcore-coopmatrix-spec.md).
- [cajeta.xpu](xpu/CajetaXPU.md) — the accelerator substrate (kernels, launch, buffers).
- [CajetaXPU-Matrix](xpu/CajetaXPU-Matrix.md) — the capability matrix across backends.
- [CajetaXPU-FFI](xpu/CajetaXPU-FFI.md) — launch and kernel-arg FFI contract.
- [CajetaXPU-Variance](xpu/CajetaXPU-Variance.md) — variance discipline.
- [CajetaCPU](xpu/CajetaCPU.md) — CPU backend and graceful degradation.
- Wave/subgroup ops (`cajeta.xpu`): [WaveReductions](xpu/WaveReductions.md),
  [WavePrefixScan](xpu/WavePrefixScan.md), [SubgroupRotate](xpu/SubgroupRotate.md),
  [QuadControl](xpu/QuadControl.md), [IntegerAtomics](xpu/IntegerAtomics.md),
  [FloatAtomics](xpu/FloatAtomics.md), [ShaderClock](xpu/ShaderClock.md).
- [cajeta.gfx](gfx/cajeta-gfx-spec.md) — graphics primitives over XPU.

## Toolchain and platform

- [BuildTool](buildtool/BuildTool.md) — the `cajeta` builder.
- [Compilation](buildtool/Compilation.md) — the compile pipeline;
  [IncrementalCompilation](buildtool/IncrementalCompilation.md),
  [CompilerModes](buildtool/CompilerModes.md).
- [LibraryProjectType](buildtool/LibraryProjectType.md) — library projects;
  [ArchiveManagement](buildtool/ArchiveManagement.md),
  [native-deps-spec](buildtool/native-deps-spec.md).
- [Documentation](buildtool/Documentation.md) — cajetadoc.
- [olla-ci-publish](buildtool/olla-ci-publish.md) — publishing to Olla from CI;
  [windows-release-ci-spec](buildtool/windows-release-ci-spec.md) — the Windows release leg.
- [Debugging](debugging/Debugging.md) — the DAP server and debug story.
- [Embedded](embedded/Embedded.md) — the embedded roadmap.
- [CajetaMcp](mcp/CajetaMcp.md) — the MCP server for the toolchain.
