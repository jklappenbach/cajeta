# xpu-cache-discriminator — the incremental cache must distinguish device backends

## 1. Definition

**1.1** The build tool's incremental compilation cache is keyed by a
**discriminator**: a hash over the flag set that decides what a compile produces.
Two builds that would emit different code must hash differently, or the second
silently reuses the first's objects.

**1.2** `--xpu-backend` and `--xpu-arch` decide which device kernels are compiled
and embedded for every `@Kernel` method. They **do not feed the discriminator**.
So a project built for one accelerator and then for another reuses the first
build's device objects, and the resulting binary contains kernels for a backend
it was not asked for.

**1.3 Measured, 2026-09-02.** `--print-cache-discriminator` already exists and
answers this in one command, with no device and no build:

| flags | discriminator |
|---|---|
| `--xpu-backend=cpu` | `63ed014a…89d0` |
| `--xpu-backend=amdgpu --xpu-arch=gfx1151` | `63ed014a…89d0` |
| `--xpu-backend=nvptx` | `63ed014a…89d0` |
| `--xpu-arch=sm_89` | `63ed014a…89d0` |
| *(no xpu flags at all)* | `63ed014a…89d0` |

Five materially different builds, one key.

**1.4 The end-to-end consequence**, on `samples/kernel-profile` (gfx1151):

```
clean -> cajeta cpu     [incremental] discriminator 3662204054b3…ee30
                        active backend: cpu
then  -> cajeta gpu     [incremental] discriminator 3662204054b3…ee30
                        [incremental] skip kernelprofile/KernelProfile.cajeta
                        active backend: cpu        <- asked for amdgpu
```

The file holding the `@Kernel` methods is **skipped**, so the GPU task emits a
binary with CPU kernels. Reversed, the `cpu` task emits one containing HIP
kernels that reports `hip`, and forcing `CAJETA_XPU_BACKEND=cpu` on it fails with
*"no available backend among {hip}"*.

**1.5 Why it is worse than a stale build.** The artifact is not identical to the
correct one — the shas differ (`48b565cf…` built clean vs `38b1bd34…` built after
the other task), because linking and output paths do differ. Only the *device
objects* are wrong. So a reader comparing artifact hashes sees two distinct
builds and concludes the toolchain did its job.

**1.6 Non-goals.**
- The runtime's backend selection order (CUDA → HIP → Vulkan → CPU). It is
  correct; it was simply choosing among the wrong embedded set.
- Cache keying for anything other than the two xpu flags.
- `samples/kernel-profile/run.sh`'s purge, which is a workaround to be removed
  once this is fixed, not the fix.

## 2. The discriminator

**2.1** When two builds differ only in `--xpu-backend`, their discriminators
differ.

**2.2** When two builds differ only in `--xpu-arch`, their discriminators differ.

**2.3** When a build passes no xpu flags, its discriminator differs from one that
passes `--xpu-backend=cpu` — "host-only" and "CPU kernels embedded" are not the
same output.

**2.4** When two builds agree on every flag including the xpu pair, their
discriminators are equal, so incremental reuse still works within one backend.

## 3. What the cache does with it

**3.1** When a project is built for one backend and then another, the source
holding `@Kernel` methods is recompiled rather than skipped.

**3.2** When the same backend is built twice with no source change, the second
build still reuses the cache — the fix must not disable incrementality.

**3.3** When a build reuses a cached object, the object was produced by a compile
whose xpu flags match the current ones.

## 4. Reporting

**4.1** When a build reuses cached objects, the reported discriminator is the one
the objects were keyed under, so two tasks printing the same discriminator means
they genuinely share a key rather than hiding a difference.

## 5. Coverage

**5.1** The discriminator assertions run with no device present: they compare
hashes from `--print-cache-discriminator`, which compiles nothing.

**5.2** At least one test builds twice in sequence and asserts the second build
did not skip the kernel-bearing source — a hash test alone would pass against a
cache that ignored the key.
