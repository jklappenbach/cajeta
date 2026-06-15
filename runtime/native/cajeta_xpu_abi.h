#ifndef CAJETA_XPU_ABI_H
#define CAJETA_XPU_ABI_H

/* Cajeta XPU — stable C ABI for kernel registration + launch.
 *
 * This header is the SINGLE SOURCE OF TRUTH for the compute FFI contract:
 * the compiler emit side (src/cajeta/xpu/...), the C runtime
 * (runtime/native/cajeta_runtime.c), and any external port (numerics /
 * PyTorch / Toffee-SPELA) all agree through the declarations here. Before
 * this header existed, the per-parameter kind values were hand-synced
 * between KernelLowering.h and the runtime's CAJETA_KP_* literals; the enum
 * below makes those numbers exist exactly once.
 *
 * The frozen contract — argv layout per parameter kind, the byteSize
 * semantics, the launch + register entry points, the device-targeting and
 * buffer-affinity rules, and which symbols are stable vs internal — is
 * documented in docs/gpu/xpu/CajetaXPU-FFI.md. Includable from both C and
 * C++ (C-linkage).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ABI version. Bumped when the contract changes in a non-backward-compatible
 * way. A launch field is added as a new symbol suffix
 * (__cajeta_xpu_launch_v2, _v3, …), never by repurposing an existing
 * argument; the parameter-kind enum is append-only (never renumbered). An
 * external caller checks __cajeta_xpu_abi_version() against this macro before
 * dispatching. */
#define CAJETA_XPU_ABI_VERSION 2

/* Per-kernel-parameter kind, in declaration order. The launch site packs one
 * argv slot per parameter; this kind tells the runtime how to read that slot
 * (and the Vulkan rung how to bind it). The numeric values are part of the
 * frozen ABI — append new kinds at the end, never renumber existing ones. */
typedef enum CajetaXpuParamKind {
    CAJETA_XPU_KP_SCALAR       = 0, /* by-value primitive/POD; byteSize bytes  */
    CAJETA_XPU_KP_BUFFER       = 1, /* Buffer<T> device handle (int64)         */
    CAJETA_XPU_KP_TEXTURE      = 2, /* Texture{1D,2D,3D,Cube,2DArray} handle   */
    CAJETA_XPU_KP_SAMPLER      = 3, /* Sampler POD {i32 filter, i32 address}   */
    CAJETA_XPU_KP_ACCEL        = 4, /* AccelerationStructure POD               */
    CAJETA_XPU_KP_IMAGE        = 5, /* Image2D storage-image handle (int64)    */
    CAJETA_XPU_KP_BUFFER_ARRAY = 6  /* Buffer<T>[] bindless: slot = [count,h0…]*/
} CajetaXpuParamKind;

/* Returns CAJETA_XPU_ABI_VERSION as compiled into the runtime — the
 * header/runtime handshake an external caller checks before dispatch. */
int32_t __cajeta_xpu_abi_version(void);

/* Launch a registered kernel (ABI v3). `argv` points to an array of one
 * pointer per kernel parameter, in declaration order, each marshalled per its
 * CajetaXpuParamKind (the runtime reads the per-kernel param metadata recorded
 * by __cajeta_xpu_register_kernel_params). `streamHandle` orders the launch on
 * a stream (0 = default). `deviceId` selects the target device: -1 = the
 * current active device; >= 0 = an index into the active backend's enumerated
 * devices (handles originate from cajeta-gpu enumeration).
 *
 * `specValues` supplies host overrides for the kernel's user specialization
 * constants (Spec.geti/getf): entry `i` overrides spec slot `i` (Vulkan SpecId
 * kFirstUserSpecId+i); `specCount` is how many leading slots are supplied
 * (trailing/unset slots keep their compile-time default). Each value is a raw
 * 4-byte word (i32 today; f32 reinterpreted later — no ABI change). NULL /
 * specCount 0 = no override (every slot reads its default; identical to v2).
 * Honored as a genuine pipeline-time spec constant on Vulkan and baked into the
 * per-value variant on CPU; AMD/NVPTX fold it into the device compile.
 * A future field is added as __cajeta_xpu_launch_v4 — never by repurposing. */
void __cajeta_xpu_launch_v3(const char* kernelName,
                            int32_t gridX, int32_t gridY, int32_t gridZ,
                            int32_t blockX, int32_t blockY, int32_t blockZ,
                            uint32_t sharedBytes, void* argv,
                            int64_t streamHandle, int32_t deviceId,
                            int32_t specCount, const int32_t* specValues);

/* Compat shim (ABI v1): equivalent to v2 with deviceId = -1. Frozen. */
void __cajeta_xpu_launch_v2(const char* kernelName,
                            int32_t gridX, int32_t gridY, int32_t gridZ,
                            int32_t blockX, int32_t blockY, int32_t blockZ,
                            uint32_t sharedBytes, void* argv,
                            int64_t streamHandle, int32_t deviceId);

/* Compat shim retained for the compiler emit path: equivalent to
 * __cajeta_xpu_launch_v2(..., deviceId = -1). Frozen signature. */
void __cajeta_xpu_launch(const char* kernelName,
                         int32_t gridX, int32_t gridY, int32_t gridZ,
                         int32_t blockX, int32_t blockY, int32_t blockZ,
                         uint32_t sharedBytes, void* argv, int64_t streamHandle);

#ifdef __cplusplus
}
#endif

#endif /* CAJETA_XPU_ABI_H */
