// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c
// === (single-TU build; not a standalone compilation unit).
// ============================================================================
// Vulkan compute binding (dlopen'd) — backs the real SPIR-V device path.
// ============================================================================
// Mirrors src/cajeta/xpu/vulkan/VulkanDriver.cpp in C. Compiled in only when a
// Vulkan SDK header is present at runtime-build time; otherwise the entry points
// are no-ops and Vulkan probes unavailable. All Vulkan functions are resolved at
// runtime via vkGetInstanceProcAddr/vkGetDeviceProcAddr (no link dependency).
#if defined(__has_include)
#  if __has_include(<vulkan/vulkan.h>)
#    define CAJETA_RT_HAS_VULKAN 1
#  endif
#endif

// Per-binding resource kind, after launch_vulkan has wrapped scalars into SSBOs.
// Defined unconditionally (not inside the Vulkan block below): the kernel-param
// dispatch loop tags every binding with one of these regardless of whether the
// real Vulkan path or the no-op stub is compiled. Keeping them out of the
// CAJETA_RT_HAS_VULKAN guard is what makes the Windows build (where that guard
// is off) link — see cajeta_xpu_vk_launch's two definitions.
#define CAJ_VKB_BUFFER  0   // bindings[i] = buffer-table handle  -> STORAGE_BUFFER
#define CAJ_VKB_TEXTURE 1   // bindings[i] = texture-table handle -> SAMPLED_IMAGE
#define CAJ_VKB_SAMPLER 2   // bindings[i] = (int64) VkSampler    -> SAMPLER
#define CAJ_VKB_ACCEL   3   // bindings[i] = accel-table handle   -> ACCELERATION_STRUCTURE_KHR
#define CAJ_VKB_STORAGE_IMAGE 4 // bindings[i] = texture-table handle (storage) -> STORAGE_IMAGE
#define CAJ_VKB_BUFFER_ARRAY  5 // bindings[i] = ptr to [int64 count, int64 h0…] -> STORAGE_BUFFER array
// Fixed bindless descriptor-array size — MUST equal the SPIR-V handlefrombinding
// `range` (kMaxBindlessBuffers in SpirvKernelLowering.cpp) and the launch
// marshalling cap (CallExpression.cpp). The layout binds this many descriptors;
// the runtime fills `count` real + pads the rest with a valid buffer.
#define CAJ_VK_BINDLESS_MAX 16

#if defined(CAJETA_RT_HAS_VULKAN)
#include <vulkan/vulkan.h>

struct cajeta_vk {
    int loaded;                  // 0 untried, 1 ready, -1 unavailable
    void* lib;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr;
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    VkQueue queue;
    uint32_t queueFamily;
    VkCommandPool cmdPool;
    VkPhysicalDeviceMemoryProperties memProps;
    PFN_vkCreateInstance vkCreateInstance;
    PFN_vkDestroyInstance vkDestroyInstance;
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
    PFN_vkCreateDevice vkCreateDevice;
    PFN_vkDestroyDevice vkDestroyDevice;
    PFN_vkGetDeviceQueue vkGetDeviceQueue;
    PFN_vkCreateBuffer vkCreateBuffer;
    PFN_vkDestroyBuffer vkDestroyBuffer;
    PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
    PFN_vkAllocateMemory vkAllocateMemory;
    PFN_vkFreeMemory vkFreeMemory;
    PFN_vkBindBufferMemory vkBindBufferMemory;
    PFN_vkMapMemory vkMapMemory;
    PFN_vkUnmapMemory vkUnmapMemory;
    // Image + sampler path (Item 8 Stage B: Texture2D / Sampler).
    PFN_vkCreateImage vkCreateImage;
    PFN_vkDestroyImage vkDestroyImage;
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
    PFN_vkBindImageMemory vkBindImageMemory;
    PFN_vkCreateImageView vkCreateImageView;
    PFN_vkDestroyImageView vkDestroyImageView;
    PFN_vkCreateSampler vkCreateSampler;
    PFN_vkDestroySampler vkDestroySampler;
    PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage;
    PFN_vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer;
    PFN_vkCmdCopyBuffer vkCmdCopyBuffer;
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
    PFN_vkCreateShaderModule vkCreateShaderModule;
    PFN_vkDestroyShaderModule vkDestroyShaderModule;
    PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout;
    PFN_vkCreatePipelineLayout vkCreatePipelineLayout;
    PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout;
    PFN_vkCreateComputePipelines vkCreateComputePipelines;
    PFN_vkDestroyPipeline vkDestroyPipeline;
    PFN_vkCreateDescriptorPool vkCreateDescriptorPool;
    PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool;
    PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets;
    PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets;
    PFN_vkCreateCommandPool vkCreateCommandPool;
    PFN_vkDestroyCommandPool vkDestroyCommandPool;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
    PFN_vkFreeCommandBuffers vkFreeCommandBuffers;
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
    PFN_vkEndCommandBuffer vkEndCommandBuffer;
    PFN_vkCmdBindPipeline vkCmdBindPipeline;
    PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets;
    PFN_vkCmdDispatch vkCmdDispatch;
    PFN_vkQueueSubmit vkQueueSubmit;
    PFN_vkQueueWaitIdle vkQueueWaitIdle;
    // Subgroup-size control (core 1.3): pin compute pipelines to wave32.
    // Kernels are AUTHORED wave32 (barrier/LDS cooperation in the staged
    // WMMA GEMMs); RADV defaults gfx11 compute to wave64, under which that
    // cooperation is barrier divergence — measured as a GPU HANG and
    // "context lost, guilty of a hard recovery" on the first Q6 batch GEMM.
    int subgroupCtl;                 // feature enabled + 32 within [min,max]
    uint32_t minSubgroupSize;
    uint32_t maxSubgroupSize;
    // Deferred batch submission (v2 launch path).
    PFN_vkCreateFence vkCreateFence;
    PFN_vkWaitForFences vkWaitForFences;
    PFN_vkResetFences vkResetFences;
    PFN_vkResetDescriptorPool vkResetDescriptorPool;
    PFN_vkResetCommandBuffer vkResetCommandBuffer;
    // Ray-query / acceleration-structure path (cajeta-gpu Part C inc 3b).
    // Resolved + the device extensions/features enabled only when the physical
    // device supports VK_KHR_acceleration_structure + VK_KHR_ray_query +
    // buffer-device-address; `rayQuery` stays 0 otherwise (and the AS natives
    // no-op) so the compute buffer/texture path is unaffected on non-RT GPUs.
    int rayQuery;                // 1 if AS/ray-query is usable on this device
    PFN_vkGetBufferDeviceAddress vkGetBufferDeviceAddress;
    PFN_vkGetAccelerationStructureBuildSizesKHR vkGetAccelerationStructureBuildSizesKHR;
    PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKHR;
    PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKHR;
    PFN_vkCmdBuildAccelerationStructuresKHR vkCmdBuildAccelerationStructuresKHR;
    PFN_vkGetAccelerationStructureDeviceAddressKHR vkGetAccelerationStructureDeviceAddressKHR;
    // minAccelerationStructureScratchOffsetAlignment — the BVH build scratch
    // device address must be rounded up to this (VUID-...-scratchData-03710).
    VkDeviceSize scratchAlign;
    // ── cajeta-profiler Unit 13: timestamp-query timing (spec §5.5, §6.5) ──
    uint32_t tsValidBits;        // the selected family's timestampValidBits
    float    tsPeriod;           // limits.timestampPeriod, ns per tick
    int      tsTimingOk;         // family can time (picker's verdict)
    int      hasHostQueryReset;  // core-1.2 feature enabled on the device
    int      hasSync2;           // core-1.3 feature enabled on the device
    int      hasCalibratedTs;    // calibrated-timestamps ext enabled
    PFN_vkCreateQueryPool vkCreateQueryPool;
    PFN_vkDestroyQueryPool vkDestroyQueryPool;
    PFN_vkCmdResetQueryPool vkCmdResetQueryPool;
    PFN_vkCmdWriteTimestamp vkCmdWriteTimestamp;
    PFN_vkGetQueryPoolResults vkGetQueryPoolResults;
#if defined(VK_VERSION_1_2)
    PFN_vkResetQueryPool vkResetQueryPool;   // host reset (core 1.2 / EXT alias)
#endif
#if defined(VK_EXT_calibrated_timestamps)
    PFN_vkGetCalibratedTimestampsEXT vkGetCalibratedTimestamps;   // KHR is ABI-identical
#endif
    VkQueryPool profPool;        // 2 queries; the dispatch path is serialized
    int      profPoolReady;      // 0 not yet, 1 ready, -1 refused
    int64_t  lastCalibrateNs;    // §6.6 refresh bookkeeping
    int      calPaired;          // 1 = DEVICE+CLOCK_MONOTONIC in one driver
                                 // read; 0 = DEVICE only, our own bracket
};
static struct cajeta_vk g_xpu_vk;

// Serializes all VkQueue submits + VkCommandPool use AND every resource-table
// mutation/read (the buffer g_vk_bufs, texture g_vk_texs, and AS g_vk_accels
// tables). A VkQueue and a VkCommandPool require external host synchronization,
// and the tables are plain arrays + counts; the launch/build/free/alloc paths can
// be driven from different OS threads (the program's main thread vs a carrier-
// fiber thread), so without this they race the shared queue/pool/tables.
// RECURSIVE: the launch path holds this across the whole dispatch and calls the
// table accessors (cajeta_xpu_vk_rec / _tex_rec) under it, so the accessors must
// be able to re-lock. Distinct from g_xpu_cuda_lock (backend init/load only).
// Initialized at runtime in cajeta_xpu_vulkan_init_locked (the portable static
// recursive initializer needs _GNU_SOURCE, which this TU doesn't set); the glibc
// recursive enum is the _NP spelling, macOS/Windows use the unsuffixed one.
#if defined(__APPLE__) || defined(_WIN32)
#  define CAJETA_MUTEX_RECURSIVE PTHREAD_MUTEX_RECURSIVE
#else
#  define CAJETA_MUTEX_RECURSIVE PTHREAD_MUTEX_RECURSIVE_NP
#endif
static pthread_mutex_t g_xpu_vk_submit_mu;

struct cajeta_vk_buf {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void* mapped;
    VkDeviceSize size;
    int live;
    // Sub-buffer view (Buffer.slice): a view slot borrows a parent's buffer/
    // memory (does NOT own them — free() must not destroy them) and carries the
    // byte offset bound into the descriptor (VkDescriptorBufferInfo.offset) and
    // folded into `mapped` for host upload/download. is_view==0 for an owner.
    int is_view;
    VkDeviceSize view_offset;
    // The mapping's CPU-read speed: HOST_CACHED reads at DRAM speed; a
    // write-combined mapping (the non-cached amdgpu types) reads at
    // ~100 MB/s, so downloads from a non-cached buffer go through a GPU
    // copy into cached staging instead (cajeta_xpu_vk_read).
    int host_cached;
};
// 65536: an 8B model holds ~2k live buffers (weights, slices, per-Linear
// outputs) and per-launch temporaries churn thousands more in flight; 4096
// filled during the FIRST 8B prefill and every later alloc — the lm_head
// output included — was dropped, reading as all-zero logits. The drop is
// loud now; the cap is a leak backstop, not a budget.
#define CAJETA_VK_MAX_BUFFERS 65536
static struct cajeta_vk_buf g_vk_bufs[CAJETA_VK_MAX_BUFFERS];
static int g_vk_buf_count;
// Rotating hint so the dead-slot scans stay O(1) once the table is dense —
// scalar-arena view slots recycle ~8 per dispatch, and a from-zero scan over
// ~2k live weight entries would cost microseconds per slot.
static int g_vk_buf_free_hint;
static int caj_vk_find_dead_slot(void) {
    for (int k = 0; k < g_vk_buf_count; ++k) {
        int i = g_vk_buf_free_hint + k;
        if (i >= g_vk_buf_count) i -= g_vk_buf_count;
        if (!g_vk_bufs[i].live) {
            g_vk_buf_free_hint = i + 1;
            return i;
        }
    }
    return -1;
}

static int cajeta_xpu_vk_find_memory_type(uint32_t typeBits,
                                          VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_xpu_vk.memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (g_xpu_vk.memProps.memoryTypes[i].propertyFlags & want) == want)
            return (int) i;
    }
    return -1;
}

// Caller holds g_xpu_cuda_lock. Bring up instance/device/queue/cmdpool.
static int cajeta_xpu_vulkan_init_locked(void) {
    if (g_xpu_vk.loaded == 1) return 1;
    if (g_xpu_vk.loaded == -1) return 0;
    g_xpu_vk.loaded = -1;

    // One-time init of the recursive submit/table mutex (this runs exactly once —
    // the tri-state above gates it — and before any buffer/texture/launch use,
    // all of which go through this init first via cajeta_xpu_active_backend).
    {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, CAJETA_MUTEX_RECURSIVE);
        pthread_mutex_init(&g_xpu_vk_submit_mu, &attr);
        pthread_mutexattr_destroy(&attr);
    }

#if defined(__APPLE__)
    // MV1: macOS has no native Vulkan ICD — load MoltenVK (Vulkan->Metal). The
    // LunarG SDK installs libvulkan.1.dylib; a bare MoltenVK install ships
    // libMoltenVK.dylib. (On-device validation is gated on Apple hardware.)
    const char* libnames[] = {"libvulkan.1.dylib", "libvulkan.dylib",
                              "libMoltenVK.dylib"};
#elif defined(_WIN32)
    // The Vulkan loader the GPU driver / Vulkan runtime installs into System32
    // (on PATH via the default DLL search). LoadLibraryA resolves it there.
    const char* libnames[] = {"vulkan-1.dll"};
#else
    const char* libnames[] = {"libvulkan.so.1", "libvulkan.so"};
#endif
    const int nlibs = (int) (sizeof(libnames) / sizeof(libnames[0]));
    for (int i = 0; i < nlibs && !g_xpu_vk.lib; ++i)
        g_xpu_vk.lib = cajeta_xpu_libopen(libnames[i]);
    if (!g_xpu_vk.lib) return 0;
    g_xpu_vk.getInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr) cajeta_xpu_libsym(g_xpu_vk.lib, "vkGetInstanceProcAddr");
    if (!g_xpu_vk.getInstanceProcAddr) return 0;

    g_xpu_vk.vkCreateInstance = (PFN_vkCreateInstance)
        g_xpu_vk.getInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    if (!g_xpu_vk.vkCreateInstance) return 0;

    VkApplicationInfo app;
    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "cajeta-xpu";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
#if defined(VK_KHR_portability_enumeration)
    // Without this the loader hides portability ICDs (MoltenVK) entirely.
    const char* instExts[1];
    PFN_vkEnumerateInstanceExtensionProperties enumInstExt =
        (PFN_vkEnumerateInstanceExtensionProperties)
        g_xpu_vk.getInstanceProcAddr(VK_NULL_HANDLE,
                                     "vkEnumerateInstanceExtensionProperties");
    if (enumInstExt) {
        uint32_t n = 0;
        enumInstExt(NULL, &n, NULL);
        if (n > 0 && n <= 512) {
            VkExtensionProperties* e = (VkExtensionProperties*)
                malloc(n * sizeof(VkExtensionProperties));
            if (e) {
                enumInstExt(NULL, &n, e);
                for (uint32_t i = 0; i < n; ++i)
                    if (strcmp(e[i].extensionName,
                               VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
                        instExts[0] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
                        ici.enabledExtensionCount = 1;
                        ici.ppEnabledExtensionNames = instExts;
                        ici.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
                    }
                free(e);
            }
        }
    }
#endif
    if (g_xpu_vk.vkCreateInstance(&ici, NULL, &g_xpu_vk.instance) != VK_SUCCESS)
        return 0;

    #define CAJ_VKI(nm) g_xpu_vk.nm = (PFN_##nm)                               \
        g_xpu_vk.getInstanceProcAddr(g_xpu_vk.instance, #nm)
    CAJ_VKI(vkDestroyInstance);
    CAJ_VKI(vkEnumeratePhysicalDevices);
    CAJ_VKI(vkGetPhysicalDeviceQueueFamilyProperties);
    CAJ_VKI(vkGetPhysicalDeviceMemoryProperties);
    CAJ_VKI(vkCreateDevice);
    CAJ_VKI(vkDestroyDevice);
    // Optional (ray-query detection): present on any 1.1+ ICD. Resolved here so
    // bring-up can probe AS/ray-query support before vkCreateDevice.
    PFN_vkEnumerateDeviceExtensionProperties enumDevExt =
        (PFN_vkEnumerateDeviceExtensionProperties) g_xpu_vk.getInstanceProcAddr(
            g_xpu_vk.instance, "vkEnumerateDeviceExtensionProperties");
    PFN_vkGetPhysicalDeviceFeatures2 getFeatures2 =
        (PFN_vkGetPhysicalDeviceFeatures2) g_xpu_vk.getInstanceProcAddr(
            g_xpu_vk.instance, "vkGetPhysicalDeviceFeatures2");
    PFN_vkGetPhysicalDeviceProperties2 getProps2 =
        (PFN_vkGetPhysicalDeviceProperties2) g_xpu_vk.getInstanceProcAddr(
            g_xpu_vk.instance, "vkGetPhysicalDeviceProperties2");
    #undef CAJ_VKI
    g_xpu_vk.getDeviceProcAddr = (PFN_vkGetDeviceProcAddr)
        g_xpu_vk.getInstanceProcAddr(g_xpu_vk.instance, "vkGetDeviceProcAddr");
    if (!g_xpu_vk.vkEnumeratePhysicalDevices || !g_xpu_vk.vkCreateDevice ||
        !g_xpu_vk.getDeviceProcAddr)
        return 0;

    uint32_t count = 0;
    g_xpu_vk.vkEnumeratePhysicalDevices(g_xpu_vk.instance, &count, NULL);
    if (count == 0) return 0;
    if (count > 16) count = 16;
    VkPhysicalDevice devs[16];
    g_xpu_vk.vkEnumeratePhysicalDevices(g_xpu_vk.instance, &count, devs);

    int found = 0;
    for (uint32_t di = 0; di < count && !found; ++di) {
        uint32_t qn = 0;
        g_xpu_vk.vkGetPhysicalDeviceQueueFamilyProperties(devs[di], &qn, NULL);
        if (qn > 32) qn = 32;
        VkQueueFamilyProperties qp[32];
        g_xpu_vk.vkGetPhysicalDeviceQueueFamilyProperties(devs[di], &qn, qp);
        // cajeta-profiler 13.2.b (spec §5.5.2): prefer a compute family whose
        // timestamps MEAN something. Three of five families on the reference
        // device report timestampValidBits == 0, where a timestamp write is
        // legal, returns a value, and means nothing. A device whose compute
        // families all report zero still dispatches — timing is refused, not
        // the device (§10.4).
        uint32_t qflags[32], qbits[32];
        for (uint32_t qi = 0; qi < qn; ++qi) {
            qflags[qi] = (uint32_t) qp[qi].queueFlags;
            qbits[qi] = qp[qi].timestampValidBits;
        }
        int32_t timingOk = 0;
        int32_t pick = __cajeta_xpu_vk_pick_queue_family(qflags, qbits,
                                                         (int32_t) qn,
                                                         &timingOk);
        if (pick >= 0) {
            g_xpu_vk.phys = devs[di];
            g_xpu_vk.queueFamily = (uint32_t) pick;
            g_xpu_vk.tsValidBits = qbits[pick];
            g_xpu_vk.tsTimingOk = timingOk;
            found = 1;
        }
    }
    if (!found) return 0;
    g_xpu_vk.vkGetPhysicalDeviceMemoryProperties(g_xpu_vk.phys,
                                                 &g_xpu_vk.memProps);

    // Ray-query probe: the BVH/ray-query path needs three device extensions
    // (acceleration_structure pulls in deferred_host_operations;
    // buffer_device_address backs the build's scratch/AABB addresses) AND the
    // matching feature bits. Enable them only when ALL are present — turning on
    // an unsupported extension fails vkCreateDevice outright, which would break
    // the plain compute path on a non-RT GPU. Absent any of them, rayQuery stays
    // 0 and the device is created exactly as before.
    // wantAtomicFloat{,2}: VK_EXT_shader_atomic_float (FAdd) and
    // VK_EXT_shader_atomic_float2 (FMin/FMax) back Buffer<float32>.atomic{Add,Min,
    // Max} (OpAtomicFAddEXT / OpAtomicF{Min,Max}EXT). NVIDIA (unlike RADV) FAULTS
    // — VK_ERROR_DEVICE_LOST — if a shader uses these without the extension+feature
    // enabled at device creation. Probed here, enabled below when supported.
    // wantAtomicInt64: VK_KHR_shader_atomic_int64 (core in 1.2) backs
    // Buffer<int64|uint64>.atomic* — 64-bit OpAtomicI* declares the Int64Atomics
    // capability, which shaderBufferInt64Atomics must be enabled to satisfy.
    int wantRayQuery = 0, wantAtomicFloat = 0, wantAtomicFloat2 = 0;
    int wantAtomicInt64 = 0;
    if (enumDevExt && getFeatures2) {
        uint32_t extCount = 0;
        enumDevExt(g_xpu_vk.phys, NULL, &extCount, NULL);
        if (extCount > 0 && extCount <= 4096) {
            VkExtensionProperties* exts = (VkExtensionProperties*) malloc(
                sizeof(VkExtensionProperties) * extCount);
            if (exts) {
                enumDevExt(g_xpu_vk.phys, NULL, &extCount, exts);
                int hasAccel = 0, hasRayQ = 0, hasDefer = 0, hasBDA = 0;
                int hasAtomicFloat = 0, hasAtomicFloat2 = 0, hasAtomicInt64 = 0;
                for (uint32_t i = 0; i < extCount; ++i) {
                    const char* en = exts[i].extensionName;
                    if (!strcmp(en, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)) hasAccel = 1;
                    else if (!strcmp(en, VK_KHR_RAY_QUERY_EXTENSION_NAME)) hasRayQ = 1;
                    else if (!strcmp(en, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)) hasDefer = 1;
                    else if (!strcmp(en, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)) hasBDA = 1;
                    else if (!strcmp(en, VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME)) hasAtomicFloat = 1;
                    else if (!strcmp(en, VK_EXT_SHADER_ATOMIC_FLOAT_2_EXTENSION_NAME)) hasAtomicFloat2 = 1;
                    else if (!strcmp(en, VK_KHR_SHADER_ATOMIC_INT64_EXTENSION_NAME)) hasAtomicInt64 = 1;
                }
                free(exts);

                // Atomic-int64 feature query (gated on the extension being
                // advertised, like the float path — enabling an extension the
                // device lacks fails vkCreateDevice outright).
                if (hasAtomicInt64) {
                    VkPhysicalDeviceShaderAtomicInt64Features ai64;
                    memset(&ai64, 0, sizeof(ai64));
                    ai64.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES;
                    VkPhysicalDeviceFeatures2 aif2;
                    memset(&aif2, 0, sizeof(aif2));
                    aif2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                    aif2.pNext = &ai64;
                    getFeatures2(g_xpu_vk.phys, &aif2);
                    if (ai64.shaderBufferInt64Atomics)
                        wantAtomicInt64 = 1;
                }

                // Atomic-float feature query: enable only the bits the device
                // advertises (the SPIR-V emit declares both add and min/max).
                if (hasAtomicFloat || hasAtomicFloat2) {
                    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT af;
                    memset(&af, 0, sizeof(af));
                    af.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT;
                    VkPhysicalDeviceShaderAtomicFloat2FeaturesEXT af2;
                    memset(&af2, 0, sizeof(af2));
                    af2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_2_FEATURES_EXT;
                    af.pNext = &af2;
                    VkPhysicalDeviceFeatures2 aff2;
                    memset(&aff2, 0, sizeof(aff2));
                    aff2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                    aff2.pNext = &af;
                    getFeatures2(g_xpu_vk.phys, &aff2);
                    if (hasAtomicFloat && af.shaderBufferFloat32AtomicAdd)
                        wantAtomicFloat = 1;
                    if (hasAtomicFloat2 && af2.shaderBufferFloat32AtomicMinMax)
                        wantAtomicFloat2 = 1;
                }
                if (hasAccel && hasRayQ && hasDefer && hasBDA) {
                    VkPhysicalDeviceRayQueryFeaturesKHR rqf;
                    memset(&rqf, 0, sizeof(rqf));
                    rqf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
                    VkPhysicalDeviceAccelerationStructureFeaturesKHR asf;
                    memset(&asf, 0, sizeof(asf));
                    asf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
                    asf.pNext = &rqf;
                    VkPhysicalDeviceBufferDeviceAddressFeatures bdaf;
                    memset(&bdaf, 0, sizeof(bdaf));
                    bdaf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
                    bdaf.pNext = &asf;
                    VkPhysicalDeviceFeatures2 f2;
                    memset(&f2, 0, sizeof(f2));
                    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                    f2.pNext = &bdaf;
                    getFeatures2(g_xpu_vk.phys, &f2);
                    if (rqf.rayQuery && asf.accelerationStructure &&
                        bdaf.bufferDeviceAddress) {
                        wantRayQuery = 1;
                        // Cache the BVH-build scratch offset alignment.
                        if (getProps2) {
                            VkPhysicalDeviceAccelerationStructurePropertiesKHR asp;
                            memset(&asp, 0, sizeof(asp));
                            asp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
                            VkPhysicalDeviceProperties2 p2;
                            memset(&p2, 0, sizeof(p2));
                            p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                            p2.pNext = &asp;
                            getProps2(g_xpu_vk.phys, &p2);
                            g_xpu_vk.scratchAlign =
                                asp.minAccelerationStructureScratchOffsetAlignment;
                        }
                    }
                }
            }
        }
    }

    // shaderInt8 probe: SPIR-V the device backend emits can declare the Int8
    // capability (e.g. byte-addressed loads, the ray-query candidate-type read),
    // which VUID-VkShaderModuleCreateInfo-pCode-08740 requires shaderInt8 to back.
    // Enable it when supported; left off (no chain) otherwise so vkCreateDevice
    // still succeeds on devices lacking it.
    // shaderInt64 (core feature) is the same story for kernels that touch 64-bit
    // ints (e.g. device handles); read it from the same features2 query.
    int wantInt8 = 0, wantInt64 = 0, wantInt16 = 0;
    // 8/16-bit storage-buffer access: kernels whose SPIR-V loads/stores bytes or
    // halves straight from a StorageBuffer (KernelBuffer<int8> element reads —
    // the per-row quant mat-vecs) declare StorageBuffer8BitAccess, which
    // VUID-RuntimeSpirv-storageBuffer8BitAccess-06328 requires these features to
    // back. RADV does not reject the module when they are missing — it crashes
    // compiling it (fault addr 0x40 in vkCreateComputePipelines). Probe each bit
    // and enable exactly what the device reports. Core in Vk 1.2+.
    VkPhysicalDevice8BitStorageFeatures qs8;
    VkPhysicalDevice16BitStorageFeatures qs16;
    memset(&qs8, 0, sizeof(qs8));
    memset(&qs16, 0, sizeof(qs16));
    if (getFeatures2) {
        VkPhysicalDeviceShaderFloat16Int8Features qi8;
        memset(&qi8, 0, sizeof(qi8));
        qi8.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
        qs8.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES;
        qs16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
        qi8.pNext = &qs8;
        qs8.pNext = &qs16;
        VkPhysicalDeviceFeatures2 qf2;
        memset(&qf2, 0, sizeof(qf2));
        qf2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        qf2.pNext = &qi8;
        getFeatures2(g_xpu_vk.phys, &qf2);
        wantInt8 = qi8.shaderInt8 ? 1 : 0;
        wantInt64 = qf2.features.shaderInt64 ? 1 : 0;
        wantInt16 = qf2.features.shaderInt16 ? 1 : 0;
#if defined(VK_VERSION_1_3)
        {
            VkPhysicalDeviceSubgroupSizeControlFeatures qssc;
            memset(&qssc, 0, sizeof(qssc));
            qssc.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES;
            VkPhysicalDeviceSubgroupSizeControlProperties pssc;
            memset(&pssc, 0, sizeof(pssc));
            pssc.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES;
            VkPhysicalDeviceFeatures2 qf2b;
            memset(&qf2b, 0, sizeof(qf2b));
            qf2b.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            qf2b.pNext = &qssc;
            getFeatures2(g_xpu_vk.phys, &qf2b);
            VkPhysicalDeviceProperties2 qp2;
            memset(&qp2, 0, sizeof(qp2));
            qp2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            qp2.pNext = &pssc;
            PFN_vkGetPhysicalDeviceProperties2 gp2 =
                (PFN_vkGetPhysicalDeviceProperties2) g_xpu_vk.getInstanceProcAddr(
                    g_xpu_vk.instance, "vkGetPhysicalDeviceProperties2");
            if (gp2) gp2(g_xpu_vk.phys, &qp2);
            g_xpu_vk.minSubgroupSize = pssc.minSubgroupSize;
            g_xpu_vk.maxSubgroupSize = pssc.maxSubgroupSize;
            g_xpu_vk.subgroupCtl = (qssc.subgroupSizeControl
                                    && pssc.minSubgroupSize <= 32u
                                    && pssc.maxSubgroupSize >= 32u) ? 1 : 0;
        }
#endif
        qs8.pNext = NULL;    // re-used below as the enable structs
        qs16.pNext = NULL;
    }

    // cajeta-profiler 13.2.a (spec §5.5): the three timing facilities, probed
    // and enabled when available, unconditional and free when unused. Host
    // query reset and synchronization2 are core features (1.2 / 1.3);
    // calibrated timestamps is an extension with an ABI-identical KHR/EXT
    // pair. None of them is load-bearing for dispatch — a device without any
    // still runs kernels, and the profiler degrades per §10.4.
    int wantHostQueryReset = 0, wantSync2 = 0, wantCalTs = 0, wantVmm = 0;
    uint32_t devApi = VK_API_VERSION_1_0;
    {
        PFN_vkGetPhysicalDeviceProperties gp =
            (PFN_vkGetPhysicalDeviceProperties) g_xpu_vk.getInstanceProcAddr(
                g_xpu_vk.instance, "vkGetPhysicalDeviceProperties");
        VkPhysicalDeviceProperties pp;
        if (gp) { gp(g_xpu_vk.phys, &pp); devApi = pp.apiVersion; }
    }
#if defined(VK_VERSION_1_2)
    if (getFeatures2 && devApi >= VK_API_VERSION_1_2) {
        VkPhysicalDeviceHostQueryResetFeatures qhr;
        memset(&qhr, 0, sizeof(qhr));
        qhr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES;
        VkPhysicalDeviceVulkanMemoryModelFeatures qvmm;
        memset(&qvmm, 0, sizeof(qvmm));
        qvmm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES;
        qhr.pNext = &qvmm;
        VkPhysicalDeviceFeatures2 qf2b;
        memset(&qf2b, 0, sizeof(qf2b));
        qf2b.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        qf2b.pNext = &qhr;
#if defined(VK_VERSION_1_3)
        VkPhysicalDeviceSynchronization2Features qs2;
        memset(&qs2, 0, sizeof(qs2));
        qs2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
        qvmm.pNext = (devApi >= VK_API_VERSION_1_3) ? (void*)&qs2 : NULL;
#endif
        getFeatures2(g_xpu_vk.phys, &qf2b);
        wantHostQueryReset = qhr.hostQueryReset ? 1 : 0;
        wantVmm = qvmm.vulkanMemoryModel ? 1 : 0;
#if defined(VK_VERSION_1_3)
        wantSync2 = (devApi >= VK_API_VERSION_1_3 && qs2.synchronization2) ? 1 : 0;
#endif
    }
#endif
    int wantPortSubset = 0;
    if (enumDevExt) {
        uint32_t extCount = 0;
        enumDevExt(g_xpu_vk.phys, NULL, &extCount, NULL);
        if (extCount > 0 && extCount <= 512) {
            VkExtensionProperties* exts = (VkExtensionProperties*)
                malloc(extCount * sizeof(VkExtensionProperties));
            if (exts) {
                enumDevExt(g_xpu_vk.phys, NULL, &extCount, exts);
                for (uint32_t e = 0; e < extCount; ++e) {
#if defined(VK_EXT_calibrated_timestamps)
                    if (strcmp(exts[e].extensionName,
                               VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) == 0)
                        wantCalTs = 1;
#endif
                    // Spec requires enabling this whenever it is advertised.
                    if (strcmp(exts[e].extensionName,
                               "VK_KHR_portability_subset") == 0)
                        wantPortSubset = 1;
                }
                free(exts);
            }
        }
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof(qci));
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = g_xpu_vk.queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;

    // Device extensions + feature chain, accumulated across the optional paths
    // (RT/ray-query, EXT_shader_atomic_float{,2}). Enabling an unsupported
    // extension fails vkCreateDevice outright, so each path is gated on its probe
    // above; absent all of them the device is created exactly as before.
    const char* devExts[12];
    uint32_t nDevExts = 0;
    VkPhysicalDeviceRayQueryFeaturesKHR enRqf;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR enAsf;
    VkPhysicalDeviceBufferDeviceAddressFeatures enBdaf;
    if (wantRayQuery) {
        devExts[nDevExts++] = VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME;
        devExts[nDevExts++] = VK_KHR_RAY_QUERY_EXTENSION_NAME;
        devExts[nDevExts++] = VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME;
        devExts[nDevExts++] = VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME;
        memset(&enRqf, 0, sizeof(enRqf));
        enRqf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        enRqf.rayQuery = VK_TRUE;
        memset(&enAsf, 0, sizeof(enAsf));
        enAsf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        enAsf.accelerationStructure = VK_TRUE;
        enAsf.pNext = &enRqf;
        memset(&enBdaf, 0, sizeof(enBdaf));
        enBdaf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
        enBdaf.bufferDeviceAddress = VK_TRUE;
        enBdaf.pNext = (void*) dci.pNext;
        dci.pNext = &enBdaf;
    }
    // EXT_shader_atomic_float{,2}: enable each independently (a feature struct for
    // an extension that isn't in the enabled list is invalid), prepending to the
    // pNext chain. Fixes Buffer<float32>.atomic{Add,Min,Max} device-loss on NVIDIA.
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT enAf;
    VkPhysicalDeviceShaderAtomicFloat2FeaturesEXT enAf2;
    if (wantAtomicFloat) {
        devExts[nDevExts++] = VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME;
        memset(&enAf, 0, sizeof(enAf));
        enAf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT;
        enAf.shaderBufferFloat32AtomicAdd = VK_TRUE;
        enAf.pNext = (void*) dci.pNext;
        dci.pNext = &enAf;
    }
    if (wantAtomicFloat2) {
        devExts[nDevExts++] = VK_EXT_SHADER_ATOMIC_FLOAT_2_EXTENSION_NAME;
        memset(&enAf2, 0, sizeof(enAf2));
        enAf2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_2_FEATURES_EXT;
        enAf2.shaderBufferFloat32AtomicMinMax = VK_TRUE;
        enAf2.pNext = (void*) dci.pNext;
        dci.pNext = &enAf2;
    }
    // KHR_shader_atomic_int64: backs Buffer<int64|uint64>.atomic* (the
    // Int64Atomics capability the 64-bit OpAtomicI* forms declare).
    VkPhysicalDeviceShaderAtomicInt64Features enAi64;
    if (wantAtomicInt64) {
        devExts[nDevExts++] = VK_KHR_SHADER_ATOMIC_INT64_EXTENSION_NAME;
        memset(&enAi64, 0, sizeof(enAi64));
        enAi64.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES;
        enAi64.shaderBufferInt64Atomics = VK_TRUE;
        enAi64.pNext = (void*) dci.pNext;
        dci.pNext = &enAi64;
    }
    if (nDevExts > 0) {
    }

    // Prepend shaderInt8 to whatever feature chain is set (the RT chain, or NULL).
    // shaderInt8 is required by the software ray-query variant ($sw), which declares
    // the SPIR-V Int8 capability (the SoftwareRayQuery walk uses byte-width values);
    // wantInt8 is set from the device's queried shaderInt8 support. Core in Vk 1.2+.
    VkPhysicalDeviceShaderFloat16Int8Features enInt8;
    if (wantInt8) {
        memset(&enInt8, 0, sizeof(enInt8));
        enInt8.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
        enInt8.shaderInt8 = VK_TRUE;
        enInt8.pNext = (void*) dci.pNext;
        dci.pNext = &enInt8;
    }
    // 8/16-bit storage: the probed structs above already hold exactly the bits
    // the device supports, so chain them as-is (a struct whose bits are all
    // VK_FALSE is legal but pointless — skip it). See the probe comment for why
    // missing these is a driver CRASH, not a clean failure.
    if (qs8.storageBuffer8BitAccess || qs8.uniformAndStorageBuffer8BitAccess
            || qs8.storagePushConstant8) {
        qs8.pNext = (void*) dci.pNext;
        dci.pNext = &qs8;
    }
    if (qs16.storageBuffer16BitAccess || qs16.uniformAndStorageBuffer16BitAccess
            || qs16.storagePushConstant16 || qs16.storageInputOutput16) {
        qs16.pNext = (void*) dci.pNext;
        dci.pNext = &qs16;
    }
#if defined(VK_VERSION_1_3)
    VkPhysicalDeviceSubgroupSizeControlFeatures enSsc;
    if (g_xpu_vk.subgroupCtl) {
        memset(&enSsc, 0, sizeof(enSsc));
        enSsc.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES;
        enSsc.subgroupSizeControl = VK_TRUE;
        enSsc.pNext = (void*) dci.pNext;
        dci.pNext = &enSsc;
    }
#endif
    // shaderInt64 is a CORE feature -> pEnabledFeatures (legal alongside the pNext
    // extension-feature chain, which carries no VkPhysicalDeviceFeatures2).
    VkPhysicalDeviceFeatures coreFeats;
    memset(&coreFeats, 0, sizeof(coreFeats));
    if (wantInt64) {
        coreFeats.shaderInt64 = VK_TRUE;
        dci.pEnabledFeatures = &coreFeats;
    }
    // shaderInt16 likewise (16-bit arithmetic — the Int16 capability kernels
    // declare for short-typed intermediates).
    if (wantInt16) {
        coreFeats.shaderInt16 = VK_TRUE;
        dci.pEnabledFeatures = &coreFeats;
    }
    // CAJETA_XPU_VK_ROBUST=1: enable robustBufferAccess (core) — the OOB
    // probe. llama.cpp's quantized tile loaders have NO M/N bounds checks and
    // are safe only because robustness clamps the overhang; if our Q6 batch
    // kernels' device-loss at engine scale is an OOB read, robustness turns
    // the hang into clamped zeros. A diagnostic arm, not the shipped config.
    if (getenv("CAJETA_XPU_VK_ROBUST")) {
        coreFeats.robustBufferAccess = VK_TRUE;
        dci.pEnabledFeatures = &coreFeats;
        fprintf(stderr, "cajeta.xpu.vulkan: robustBufferAccess ENABLED "
                "(probe arm)\n");
    }

    // cajeta-profiler 13.2.a — the timing facilities, prepended to the chain.
#if defined(VK_VERSION_1_2)
    VkPhysicalDeviceHostQueryResetFeatures enHqr;
    if (wantHostQueryReset) {
        memset(&enHqr, 0, sizeof(enHqr));
        enHqr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES;
        enHqr.hostQueryReset = VK_TRUE;
        enHqr.pNext = (void*) dci.pNext;
        dci.pNext = &enHqr;
    }
#endif
#if defined(VK_VERSION_1_3)
    VkPhysicalDeviceSynchronization2Features enS2;
    if (wantSync2) {
        memset(&enS2, 0, sizeof(enS2));
        enS2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
        enS2.synchronization2 = VK_TRUE;
        enS2.pNext = (void*) dci.pNext;
        dci.pNext = &enS2;
    }
#endif
    if (wantPortSubset) devExts[nDevExts++] = "VK_KHR_portability_subset";
#if defined(VK_VERSION_1_2)
    VkPhysicalDeviceVulkanMemoryModelFeatures enVmm;
    if (wantVmm) {
        memset(&enVmm, 0, sizeof(enVmm));
        enVmm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES;
        enVmm.vulkanMemoryModel = VK_TRUE;
        enVmm.pNext = (void*) dci.pNext;
        dci.pNext = &enVmm;
    }
#endif
#if defined(VK_EXT_calibrated_timestamps)
    if (wantCalTs) {
        devExts[nDevExts++] = VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
    }
#endif

    dci.enabledExtensionCount = nDevExts;
    dci.ppEnabledExtensionNames = nDevExts ? devExts : NULL;

    if (g_xpu_vk.vkCreateDevice(g_xpu_vk.phys, &dci, NULL, &g_xpu_vk.device)
            != VK_SUCCESS)
        return 0;
    g_xpu_vk.hasHostQueryReset = wantHostQueryReset;
    g_xpu_vk.hasSync2 = wantSync2;
    g_xpu_vk.hasCalibratedTs = wantCalTs;

    #define CAJ_VKD(nm) g_xpu_vk.nm = (PFN_##nm)                               \
        g_xpu_vk.getDeviceProcAddr(g_xpu_vk.device, #nm)
    CAJ_VKD(vkGetDeviceQueue);
    CAJ_VKD(vkCreateBuffer);
    CAJ_VKD(vkDestroyBuffer);
    CAJ_VKD(vkGetBufferMemoryRequirements);
    CAJ_VKD(vkAllocateMemory);
    CAJ_VKD(vkFreeMemory);
    CAJ_VKD(vkBindBufferMemory);
    CAJ_VKD(vkMapMemory);
    CAJ_VKD(vkUnmapMemory);
    CAJ_VKD(vkCreateImage);
    CAJ_VKD(vkDestroyImage);
    CAJ_VKD(vkGetImageMemoryRequirements);
    CAJ_VKD(vkBindImageMemory);
    CAJ_VKD(vkCreateImageView);
    CAJ_VKD(vkDestroyImageView);
    CAJ_VKD(vkCreateSampler);
    CAJ_VKD(vkDestroySampler);
    CAJ_VKD(vkCmdCopyBufferToImage);
    CAJ_VKD(vkCmdCopyImageToBuffer);
    CAJ_VKD(vkCmdCopyBuffer);
    CAJ_VKD(vkCmdPipelineBarrier);
    CAJ_VKD(vkCreateShaderModule);
    CAJ_VKD(vkDestroyShaderModule);
    CAJ_VKD(vkCreateDescriptorSetLayout);
    CAJ_VKD(vkDestroyDescriptorSetLayout);
    CAJ_VKD(vkCreatePipelineLayout);
    CAJ_VKD(vkDestroyPipelineLayout);
    CAJ_VKD(vkCreateComputePipelines);
    CAJ_VKD(vkDestroyPipeline);
    CAJ_VKD(vkCreateDescriptorPool);
    CAJ_VKD(vkDestroyDescriptorPool);
    CAJ_VKD(vkAllocateDescriptorSets);
    CAJ_VKD(vkUpdateDescriptorSets);
    CAJ_VKD(vkCreateCommandPool);
    CAJ_VKD(vkDestroyCommandPool);
    CAJ_VKD(vkAllocateCommandBuffers);
    CAJ_VKD(vkFreeCommandBuffers);
    CAJ_VKD(vkBeginCommandBuffer);
    CAJ_VKD(vkEndCommandBuffer);
    CAJ_VKD(vkCmdBindPipeline);
    CAJ_VKD(vkCmdBindDescriptorSets);
    CAJ_VKD(vkCmdDispatch);
    CAJ_VKD(vkQueueSubmit);
    CAJ_VKD(vkQueueWaitIdle);
    CAJ_VKD(vkCreateFence);
    CAJ_VKD(vkWaitForFences);
    CAJ_VKD(vkResetFences);
    CAJ_VKD(vkResetDescriptorPool);
    CAJ_VKD(vkResetCommandBuffer);
    // cajeta-profiler Unit 13 — timestamp-query timing entry points.
    CAJ_VKD(vkCreateQueryPool);
    CAJ_VKD(vkDestroyQueryPool);
    CAJ_VKD(vkCmdResetQueryPool);
    CAJ_VKD(vkCmdWriteTimestamp);
    CAJ_VKD(vkGetQueryPoolResults);
#if defined(VK_VERSION_1_2)
    if (g_xpu_vk.hasHostQueryReset) {
        CAJ_VKD(vkResetQueryPool);
        if (!g_xpu_vk.vkResetQueryPool)   // 1.1 device exposing only the EXT
            g_xpu_vk.vkResetQueryPool = (PFN_vkResetQueryPool)
                g_xpu_vk.getDeviceProcAddr(g_xpu_vk.device,
                                           "vkResetQueryPoolEXT");
        if (!g_xpu_vk.vkResetQueryPool) g_xpu_vk.hasHostQueryReset = 0;
    }
#endif
#if defined(VK_EXT_calibrated_timestamps)
    if (g_xpu_vk.hasCalibratedTs) {
        // KHR first (ABI-identical), EXT as the long-standing fallback.
        g_xpu_vk.vkGetCalibratedTimestamps = (PFN_vkGetCalibratedTimestampsEXT)
            g_xpu_vk.getDeviceProcAddr(g_xpu_vk.device,
                                       "vkGetCalibratedTimestampsKHR");
        if (!g_xpu_vk.vkGetCalibratedTimestamps)
            g_xpu_vk.vkGetCalibratedTimestamps = (PFN_vkGetCalibratedTimestampsEXT)
                g_xpu_vk.getDeviceProcAddr(g_xpu_vk.device,
                                           "vkGetCalibratedTimestampsEXT");
        if (!g_xpu_vk.vkGetCalibratedTimestamps) g_xpu_vk.hasCalibratedTs = 0;
    }
    if (g_xpu_vk.hasCalibratedTs) {
        // §6.5 — verify the DOMAINS, not just the extension. Passing a domain
        // the driver never offered is invalid usage that can return
        // VK_SUCCESS carrying junk. Measured on the first PHOENIX shakedown
        // (run 32755371649): Windows/NVIDIA offers QPC, not CLOCK_MONOTONIC —
        // the unchecked (DEVICE, CLOCK_MONOTONIC) read "succeeded", the
        // engine fit a host value of garbage, and every device span
        // converted to 0..0.
        //
        // Where CLOCK_MONOTONIC IS offered, the driver's paired read is the
        // tight sandwich (both clocks read close together, maxDeviation
        // stated). Where it is not (§6.8: Windows offers QPC), the DEVICE
        // domain alone is still calibrateable: the read is bracketed by our
        // own host-clock reads instead — a wider sandwich the clock engine's
        // dispersion cap still accepts, and one that never depends on any
        // assumed equivalence between QPC's epoch and the host clock's.
        PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT getDomains =
            (PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT)
                g_xpu_vk.getInstanceProcAddr(
                    g_xpu_vk.instance,
                    "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR");
        if (!getDomains)
            getDomains = (PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT)
                g_xpu_vk.getInstanceProcAddr(
                    g_xpu_vk.instance,
                    "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT");
        int haveDevice = 0, haveMonotonic = 0;
        if (getDomains) {
            uint32_t nd = 0;
            getDomains(g_xpu_vk.phys, &nd, NULL);
            if (nd > 0 && nd <= 16) {
                VkTimeDomainEXT doms[16];
                getDomains(g_xpu_vk.phys, &nd, doms);
                for (uint32_t d = 0; d < nd; ++d) {
                    if (doms[d] == VK_TIME_DOMAIN_DEVICE_EXT) haveDevice = 1;
                    if (doms[d] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT)
                        haveMonotonic = 1;
                }
            }
        }
        if (!haveDevice) g_xpu_vk.hasCalibratedTs = 0;
        g_xpu_vk.calPaired = haveMonotonic;
    }
#endif
    // RT path: resolve the AS/device-address entry points only when the device
    // was created with the ray-query extensions. vkGetBufferDeviceAddress is
    // core 1.2; the AS builders are KHR. If any fails to resolve, drop back to
    // the plain compute path (rayQuery stays 0).
    if (wantRayQuery) {
        CAJ_VKD(vkGetBufferDeviceAddress);
        if (!g_xpu_vk.vkGetBufferDeviceAddress)
            // Core 1.2 name absent (e.g. a 1.1 device exposing only the KHR
            // extension): fall back to the ABI-identical KHR entry point.
            g_xpu_vk.vkGetBufferDeviceAddress = (PFN_vkGetBufferDeviceAddress)
                g_xpu_vk.getDeviceProcAddr(g_xpu_vk.device,
                                           "vkGetBufferDeviceAddressKHR");
        CAJ_VKD(vkGetAccelerationStructureBuildSizesKHR);
        CAJ_VKD(vkCreateAccelerationStructureKHR);
        CAJ_VKD(vkDestroyAccelerationStructureKHR);
        CAJ_VKD(vkCmdBuildAccelerationStructuresKHR);
        CAJ_VKD(vkGetAccelerationStructureDeviceAddressKHR);
        g_xpu_vk.rayQuery =
            g_xpu_vk.vkGetBufferDeviceAddress &&
            g_xpu_vk.vkGetAccelerationStructureBuildSizesKHR &&
            g_xpu_vk.vkCreateAccelerationStructureKHR &&
            g_xpu_vk.vkDestroyAccelerationStructureKHR &&
            g_xpu_vk.vkCmdBuildAccelerationStructuresKHR &&
            g_xpu_vk.vkGetAccelerationStructureDeviceAddressKHR ? 1 : 0;
    }
    #undef CAJ_VKD

    g_xpu_vk.vkGetDeviceQueue(g_xpu_vk.device, g_xpu_vk.queueFamily, 0,
                              &g_xpu_vk.queue);
    VkCommandPoolCreateInfo cpci;
    memset(&cpci, 0, sizeof(cpci));
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = g_xpu_vk.queueFamily;
    if (g_xpu_vk.vkCreateCommandPool(g_xpu_vk.device, &cpci, NULL,
                                     &g_xpu_vk.cmdPool) != VK_SUCCESS)
        return 0;

    // cajeta-profiler Unit 13 — hand the timing parameters to the profiler's
    // pure half. timestampPeriod is the driver's ADVERTISED rate; §6.6's
    // rolling fit refines it (the reference device drifts −15 ppm against its
    // own advertisement), but conversions before the first calibration need a
    // seed, and §11.4 validates it rather than trusting it.
    {
        float period = 0.0f;
        if (getProps2) {
            VkPhysicalDeviceProperties2 p2;
            memset(&p2, 0, sizeof(p2));
            p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            getProps2(g_xpu_vk.phys, &p2);
            period = p2.properties.limits.timestampPeriod;
        }
        g_xpu_vk.tsPeriod = period;
        __cajeta_prof_vk_configure(g_xpu_vk.tsTimingOk ? g_xpu_vk.tsValidBits
                                                       : 0,
                                   (double) period, g_xpu_vk.hasCalibratedTs);
    }
    g_xpu_vk.loaded = 1;
    return 1;
}

// ── cajeta-profiler Unit 13: calibration + the dispatch bracket ──────────
//
// The clock engine (Unit 9) owns quality rejection, the bounded retry, drift
// fitting and snapshots; this backend only supplies the sandwich (§6.7). The
// sandwich comes from vkGetCalibratedTimestamps{KHR,EXT} with the DEVICE and
// CLOCK_MONOTONIC domains requested EXPLICITLY — the driver's own preference
// order puts CLOCK_MONOTONIC_RAW first, which §6.5 measured 5.68 s away from
// the domain the ROCm lane uses, and accepting each backend's default would
// put the two lanes seconds apart with nothing reporting an error. The
// returned maxDeviation IS the sandwich width.
#if defined(VK_EXT_calibrated_timestamps)
static int32_t caj_vk_calibration_read(int64_t* hostBeforeNs, int64_t* devTicks,
                                       int64_t* hostAfterNs, void* user) {
    (void) user;
    if (!g_xpu_vk.vkGetCalibratedTimestamps) return 0;
    if (g_xpu_vk.calPaired) {
        // The driver reads both clocks close together and states how far
        // apart the reads could have been — maxDeviation IS the sandwich.
        VkCalibratedTimestampInfoEXT infos[2];
        memset(infos, 0, sizeof(infos));
        infos[0].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT;
        infos[0].timeDomain = VK_TIME_DOMAIN_DEVICE_EXT;
        infos[1].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT;
        infos[1].timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT;
        uint64_t ts[2] = {0, 0};
        uint64_t maxDev = 0;
        if (g_xpu_vk.vkGetCalibratedTimestamps(g_xpu_vk.device, 2, infos, ts,
                                               &maxDev) != VK_SUCCESS)
            return 0;
        *devTicks = (int64_t) ts[0];
        *hostBeforeNs = (int64_t) ts[1] - (int64_t) maxDev;
        *hostAfterNs = (int64_t) ts[1] + (int64_t) maxDev;
        return 1;
    }
    // No CLOCK_MONOTONIC domain (§6.8: Windows offers QPC): read the DEVICE
    // domain alone and let OUR host clock bracket the call. Wider than the
    // driver's pairing, but in the right domain by construction — the quality
    // gate (§6.7) rejects any read the scheduler stretched too far.
    VkCalibratedTimestampInfoEXT info;
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT;
    info.timeDomain = VK_TIME_DOMAIN_DEVICE_EXT;
    uint64_t ts = 0;
    uint64_t maxDev = 0;
    const int64_t before = __cajeta_currentTimeNanos();
    if (g_xpu_vk.vkGetCalibratedTimestamps(g_xpu_vk.device, 1, &info, &ts,
                                           &maxDev) != VK_SUCCESS)
        return 0;
    const int64_t after = __cajeta_currentTimeNanos();
    if (!before || !after) return 0;
    *devTicks = (int64_t) ts;
    *hostBeforeNs = before;
    *hostAfterNs = after;
    return 1;
}
#endif

// What device creation actually enabled (plan 13.1.a) — readable so a test
// can assert the facilities were adopted where the device offers them,
// instead of trusting that the probe ran.
int32_t __cajeta_xpu_vk_has_host_query_reset(void) { return g_xpu_vk.hasHostQueryReset; }
int32_t __cajeta_xpu_vk_has_sync2(void)            { return g_xpu_vk.hasSync2; }
int32_t __cajeta_xpu_vk_has_calibrated_ts(void)    { return g_xpu_vk.hasCalibratedTs; }

// (Re)calibrate the Vulkan clock domain. Cheap enough to refresh: §6.6
// measured −15 ppm of drift (~54 ms/hour), so a single startup calibration is
// stale within seconds at microsecond span lengths.
#define CAJ_VK_RECAL_INTERVAL_NS (5LL * 1000000000LL)

static void caj_vk_calibrate_now(int64_t hostNowNs) {
#if defined(VK_EXT_calibrated_timestamps)
    if (!g_xpu_vk.hasCalibratedTs || !__cajeta_prof_vk_timing_ok()) return;
    __cajeta_prof_clock_calibrate(CAJ_GPU_BACKEND_VULKAN, caj_vk_calibration_read,
                                  NULL, /*wantSamples=*/4, /*maxAttempts=*/16);
    g_xpu_vk.lastCalibrateNs = hostNowNs;
#else
    (void) hostNowNs;
#endif
}

// Allocate a mapped storage buffer; return a 1-based table handle (0 on
// failure). Reuses a dead slot if one is free. `forStaging` selects the
// memory-preference chain: general buffers want DEVICE_LOCAL first (on the
// UMA part that is full GPU speed AND host-mappable; heap 1 is 64 GiB on
// Strix Halo), while download staging wants HOST_CACHED first so the CPU
// can read the result at DRAM speed. Every chain ends at plain
// HOST_VISIBLE|HOST_COHERENT, the v1 behaviour. CAJETA_XPU_VK_NODEVLOCAL=1
// removes the device-local preference (the control arm).
static int64_t caj_vk_alloc_pref(uint64_t bytes, int forStaging) {
    if (bytes == 0) return 0;
    static int s_devlocal = -1;
    if (s_devlocal < 0)
        s_devlocal = getenv("CAJETA_XPU_VK_NODEVLOCAL") ? 0 : 1;
    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf = VK_NULL_HANDLE;
    if (g_xpu_vk.vkCreateBuffer(g_xpu_vk.device, &bci, NULL, &buf) != VK_SUCCESS) {
        fprintf(stderr, "cajeta.xpu.vulkan: vkCreateBuffer FAILED (%llu bytes)\n",
                (unsigned long long) bytes);
        return 0;
    }
    VkMemoryRequirements req;
    memset(&req, 0, sizeof(req));
    g_xpu_vk.vkGetBufferMemoryRequirements(g_xpu_vk.device, buf, &req);
    VkMemoryPropertyFlags chain[3];
    int nchain = 0;
    if (!forStaging && s_devlocal)
        chain[nchain++] = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    chain[nchain++] = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                      VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    chain[nchain++] = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    int mtPicked = -1;
    for (int ci = 0; ci < nchain && mem == VK_NULL_HANDLE; ++ci) {
        int mt = cajeta_xpu_vk_find_memory_type(req.memoryTypeBits, chain[ci]);
        if (mt < 0) continue;
        VkMemoryAllocateInfo mai;
        memset(&mai, 0, sizeof(mai));
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = (uint32_t) mt;
        if (g_xpu_vk.vkAllocateMemory(g_xpu_vk.device, &mai, NULL, &mem)
                == VK_SUCCESS)
            mtPicked = mt;
        else
            mem = VK_NULL_HANDLE;   // budget miss: try the next tier
    }
    if (mem == VK_NULL_HANDLE) {
        fprintf(stderr, "cajeta.xpu.vulkan: vkAllocateMemory FAILED in every "
                "memory tier (%llu bytes)\n", (unsigned long long) req.size);
        g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, buf, NULL);
        return 0;
    }
    int hostCached =
        (g_xpu_vk.memProps.memoryTypes[mtPicked].propertyFlags &
         VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0;
    if (g_xpu_vk.vkBindBufferMemory(g_xpu_vk.device, buf, mem, 0) != VK_SUCCESS) {
        g_xpu_vk.vkFreeMemory(g_xpu_vk.device, mem, NULL);   // L5: don't leave a
        g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, buf, NULL);// live unbacked slot
        return 0;
    }
    void* mapped = NULL;
    if (g_xpu_vk.vkMapMemory(g_xpu_vk.device, mem, 0, VK_WHOLE_SIZE, 0, &mapped)
            != VK_SUCCESS) {
        g_xpu_vk.vkFreeMemory(g_xpu_vk.device, mem, NULL);
        g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, buf, NULL);
        return 0;
    }
    pthread_mutex_lock(&g_xpu_vk_submit_mu);   // g_vk_bufs table RMW
    int slot = caj_vk_find_dead_slot();
    if (slot < 0) {
        if (g_vk_buf_count >= CAJETA_VK_MAX_BUFFERS) {
            pthread_mutex_unlock(&g_xpu_vk_submit_mu);
            fprintf(stderr, "cajeta.xpu.vulkan: buffer table FULL "
                    "(%d) — %llu-byte alloc dropped\n",
                    CAJETA_VK_MAX_BUFFERS, (unsigned long long) bytes);
            g_xpu_vk.vkUnmapMemory(g_xpu_vk.device, mem);
            g_xpu_vk.vkFreeMemory(g_xpu_vk.device, mem, NULL);
            g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, buf, NULL);
            return 0;
        }
        slot = g_vk_buf_count++;
    }
    g_vk_bufs[slot].buffer = buf;
    g_vk_bufs[slot].memory = mem;
    g_vk_bufs[slot].mapped = mapped;
    g_vk_bufs[slot].size = bytes;
    g_vk_bufs[slot].live = 1;
    g_vk_bufs[slot].is_view = 0;        // an owner, not a slice view
    g_vk_bufs[slot].view_offset = 0;
    g_vk_bufs[slot].host_cached = hostCached;
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
    return (int64_t) (slot + 1);
}
static int64_t cajeta_xpu_vk_alloc(uint64_t bytes) {
    return caj_vk_alloc_pref(bytes, /*forStaging=*/0);
}

// Buffer.slice on Vulkan: the handle is a buffer-table index, not a pointer, so
// the byte offset can't be folded into it. Instead allocate a *view* slot that
// borrows the parent's VkBuffer/VkDeviceMemory and records the byte offset; the
// descriptor-bind path emits it as VkDescriptorBufferInfo.offset and host
// transfers see it via the offset-folded `mapped`. The view never owns the
// underlying resources — freeing a view slot clears the slot only.
// NOTE: not yet device-verified (increment B). VkDescriptorBufferInfo.offset
// must be a multiple of minStorageBufferOffsetAlignment; a caller slicing at an
// unaligned element offset will need that handled when the Vulkan path is
// brought up on hardware.
static int64_t cajeta_xpu_vk_slice(int64_t parent, uint64_t byteOffset) {
    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    struct cajeta_vk_buf* p = (parent > 0 && parent <= g_vk_buf_count &&
                               g_vk_bufs[parent - 1].live)
                                  ? &g_vk_bufs[parent - 1] : NULL;
    if (!p) { pthread_mutex_unlock(&g_xpu_vk_submit_mu); return 0; }
    VkDeviceSize base_off = p->view_offset + (VkDeviceSize) byteOffset;
    int slot = caj_vk_find_dead_slot();
    if (slot < 0) {
        if (g_vk_buf_count >= CAJETA_VK_MAX_BUFFERS) {
            pthread_mutex_unlock(&g_xpu_vk_submit_mu); return 0;
        }
        slot = g_vk_buf_count++;
    }
    g_vk_bufs[slot].buffer = p->buffer;          // borrowed, not owned
    g_vk_bufs[slot].memory = p->memory;
    g_vk_bufs[slot].mapped = p->mapped ? (void*) ((char*) p->mapped + base_off)
                                       : NULL;
    g_vk_bufs[slot].size = (p->size > base_off) ? p->size - base_off : 0;
    g_vk_bufs[slot].live = 1;
    g_vk_bufs[slot].is_view = 1;
    g_vk_bufs[slot].view_offset = base_off;
    g_vk_bufs[slot].host_cached = p->host_cached;
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
    return (int64_t) (slot + 1);
}

// rec/mapped/free take g_xpu_vk_submit_mu (recursive) so the table is read/written
// consistently even when called from a context that already holds it (vk_launch).
static struct cajeta_vk_buf* cajeta_xpu_vk_rec(int64_t handle) {
    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    struct cajeta_vk_buf* r = NULL;
    if (handle > 0 && handle <= g_vk_buf_count && g_vk_bufs[handle - 1].live)
        r = &g_vk_bufs[handle - 1];
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
    return r;
}
static void* cajeta_xpu_vk_mapped(int64_t handle) {
    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    struct cajeta_vk_buf* r = cajeta_xpu_vk_rec(handle);
    void* m = r ? r->mapped : NULL;
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
    return m;
}
// ---- Deferred batch submission state (HIP-stream parity) -------------------
// v1 recorded one command buffer per launch, submitted it and vkQueueWaitIdle'd
// under the mutex — the host blocked for the full kernel duration on every
// dispatch (~230/token on the 8B decode), which the profiler files as
// vulkan-dispatch-serialization and the bench measured as most of ~200 ms/tok.
// v2 records NoSync dispatches into ONE command buffer (compute->compute
// barrier between them) and submits once with a fence at the points that need
// results: KernelStream.sync, any host read of a buffer the batch touched, or
// a host WRITE into one (the upload-into-staging hazard HIP orders implicitly
// via stream semantics and Vulkan does not). Transient buffers/samplers freed
// while a batch is open are deferred to the flush — their descriptors are
// baked into recorded dispatches. Profiled launches (the timestamp bracket
// needs per-dispatch completion) and CAJETA_XPU_VK_SUBMIT=eager keep the v1
// per-launch path.
struct caj_vk_dpool { VkDescriptorPool pool; };
#define CAJ_VK_BATCH_POOLS 64
#define CAJ_VK_BOUND_SLOTS 8192
#define CAJ_VK_PENDING_FREES 16384
#define CAJ_VK_PENDING_SAMPLERS 256
// Chunked submission: rather than one giant submit at flush, the open batch
// is handed to the GPU every CAJ_VK_CHUNK_DISPATCHES dispatches (llama.cpp
// submits every ~100 nodes for the same reason) — the GPU executes early
// layers while the host records later ones, and no single submit is large
// enough to trip RADV's hang detection. Chunks fly fenceless; the flush's
// final submit carries the fence, and queue order makes that fence cover
// everything. CAJETA_XPU_VK_NOCHUNK=1 restores the single-submit batch.
#define CAJ_VK_BATCH_CMDS 32
#define CAJ_VK_CHUNK_DISPATCHES 100
static struct {
    int open;                      // recording?
    int inited;                    // cmds/fence created?
    unsigned dispatches;           // recorded in the open batch (all chunks)
    unsigned chunkDispatches;      // recorded in the open chunk
    unsigned chunksFlown;          // fenceless submits since last flush
    int cur;                       // ring index of the open command buffer
    VkCommandBuffer cmds[CAJ_VK_BATCH_CMDS];
    VkFence fence;
    struct caj_vk_dpool pools[CAJ_VK_BATCH_POOLS];
    int npools;                    // pools created (kept across batches)
    int poolCursor;                // pool currently allocating from
    // Open-addressing set of VkBuffer handles the open batch references.
    // Overflow degrades to "assume referenced" (flush on any host access).
    void* bound[CAJ_VK_BOUND_SLOTS];
    unsigned nbound;
    int boundOverflow;
    int64_t pendingFree[CAJ_VK_PENDING_FREES];
    int npendingFree;
    int64_t pendingSampler[CAJ_VK_PENDING_SAMPLERS];
    int npendingSampler;
} g_vk_batch;

static void cajeta_xpu_vk_flush(void);          // defined with the launch path
static void cajeta_xpu_vk_free_now(int64_t handle);

static void caj_vk_bound_note(void* buf) {
    if (!buf || g_vk_batch.boundOverflow) return;
    if (g_vk_batch.nbound > (CAJ_VK_BOUND_SLOTS * 3u) / 4u) {
        g_vk_batch.boundOverflow = 1;
        return;
    }
    size_t h = ((size_t) buf >> 4) % CAJ_VK_BOUND_SLOTS;
    while (g_vk_batch.bound[h]) {
        if (g_vk_batch.bound[h] == buf) return;
        h = (h + 1) % CAJ_VK_BOUND_SLOTS;
    }
    g_vk_batch.bound[h] = buf;
    g_vk_batch.nbound++;
}
static int caj_vk_bound_has(void* buf) {
    if (!buf) return 0;
    if (g_vk_batch.boundOverflow) return 1;
    size_t h = ((size_t) buf >> 4) % CAJ_VK_BOUND_SLOTS;
    while (g_vk_batch.bound[h]) {
        if (g_vk_batch.bound[h] == buf) return 1;
        h = (h + 1) % CAJ_VK_BOUND_SLOTS;
    }
    return 0;
}

// Host is about to READ or WRITE the mapped storage of `handle`. If the open
// batch references that VkBuffer (through any view of it — the set stores the
// VkBuffer, which a view shares with its parent), the batch must execute
// first: a read needs its results, a write would clobber a recorded-but-
// unexecuted dispatch's input. Not referenced -> no ordering owed, no flush.
static void cajeta_xpu_vk_note_host_access(int64_t handle) {
    if (!g_vk_batch.open) return;
    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    struct cajeta_vk_buf* r = cajeta_xpu_vk_rec(handle);
    if (r && g_vk_batch.open && caj_vk_bound_has((void*) r->buffer))
        cajeta_xpu_vk_flush();
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
}

// ---- Scalar-argument arena -------------------------------------------------
// Every scalar kernel argument used to allocate its own VkBuffer +
// VkDeviceMemory + map per launch (~5-20 us each, several per dispatch — the
// bulk of the ~78 us host record cost per dispatch). Instead: one persistent
// mapped arena, bump-allocated 256-byte slots bound as view slots.
// begin_launch() guarantees one launch's scalars never straddle a rewind:
// when headroom is short it flushes (fence-waited, so every recorded dispatch
// has consumed its slots) and the flush rewinds the cursor.
#define CAJ_VK_SCALAR_ARENA_BYTES (16ull * 1024u * 1024u)
#define CAJ_VK_SCALAR_SLOT 256u
static struct { int64_t handle; uint64_t cur; } g_vk_scalar_arena;
static void cajeta_xpu_vk_scalar_begin_launch(void) {
    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    if (g_vk_scalar_arena.handle &&
        g_vk_scalar_arena.cur + 64ull * CAJ_VK_SCALAR_SLOT
            > CAJ_VK_SCALAR_ARENA_BYTES)
        cajeta_xpu_vk_flush();          // rewinds the cursor
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
}
static int64_t cajeta_xpu_vk_scalar_push(const void* p, uint32_t sz) {
    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    if (!g_vk_scalar_arena.handle) {
        g_vk_scalar_arena.handle =
            caj_vk_alloc_pref(CAJ_VK_SCALAR_ARENA_BYTES, 0);
        g_vk_scalar_arena.cur = 0;
    }
    int64_t v = 0;
    if (g_vk_scalar_arena.handle && sz <= CAJ_VK_SCALAR_SLOT &&
        g_vk_scalar_arena.cur + CAJ_VK_SCALAR_SLOT
            <= CAJ_VK_SCALAR_ARENA_BYTES) {
        v = cajeta_xpu_vk_slice(g_vk_scalar_arena.handle,
                                g_vk_scalar_arena.cur);
        if (v) {
            void* m = cajeta_xpu_vk_mapped(v);
            if (m) memcpy(m, p, sz);
            g_vk_scalar_arena.cur += CAJ_VK_SCALAR_SLOT;
        }
    }
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
    return v;
}

// Read `bytes` from a buffer into host memory. A cached mapping memcpys
// straight out. A write-combined mapping (every non-HOST_CACHED amdgpu type)
// reads at ~100 MB/s — 5.2 ms for one token's logits — so a non-cached
// source goes through vkCmdCopyBuffer into a persistent HOST_CACHED staging
// buffer first: on the UMA part that copy runs at DRAM speed. Callers
// order against the open batch (note_host_access) BEFORE calling.
static int64_t g_vk_read_staging = 0;
static uint64_t g_vk_read_staging_size = 0;
static void cajeta_xpu_vk_read(int64_t handle, void* dst, uint64_t bytes) {
    static int s_readLog = -1;
    if (s_readLog < 0) s_readLog = getenv("CAJETA_XPU_VK_READ_LOG") ? 1 : 0;
    struct cajeta_vk_buf* r = cajeta_xpu_vk_rec(handle);
    if (!r || !dst || bytes == 0) return;
    if (bytes > r->size) bytes = r->size;
    if (r->host_cached && r->mapped) {
        struct timespec t0, t1;
        if (s_readLog) clock_gettime(CLOCK_MONOTONIC, &t0);
        memcpy(dst, r->mapped, (size_t) bytes);
        if (s_readLog) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            fprintf(stderr, "vk-read: cached-direct %llu B %.1f us\n",
                    (unsigned long long) bytes,
                    (t1.tv_sec - t0.tv_sec) * 1e6 +
                        (t1.tv_nsec - t0.tv_nsec) * 1e-3);
        }
        return;
    }
    struct timespec ts0, ts1, ts2;
    if (s_readLog) clock_gettime(CLOCK_MONOTONIC, &ts0);
    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    if (g_vk_read_staging_size < bytes) {
        if (g_vk_read_staging) cajeta_xpu_vk_free_now(g_vk_read_staging);
        g_vk_read_staging = caj_vk_alloc_pref(bytes, /*forStaging=*/1);
        g_vk_read_staging_size = g_vk_read_staging ? bytes : 0;
    }
    struct cajeta_vk_buf* sb =
        g_vk_read_staging ? cajeta_xpu_vk_rec(g_vk_read_staging) : NULL;
    int copied = 0;
    if (sb) {
        VkCommandBufferAllocateInfo cbai;
        memset(&cbai, 0, sizeof(cbai));
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = g_xpu_vk.cmdPool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (g_xpu_vk.vkAllocateCommandBuffers(g_xpu_vk.device, &cbai, &cmd)
                == VK_SUCCESS) {
            VkCommandBufferBeginInfo cbbi;
            memset(&cbbi, 0, sizeof(cbbi));
            cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            g_xpu_vk.vkBeginCommandBuffer(cmd, &cbbi);
            VkMemoryBarrier mb;
            memset(&mb, 0, sizeof(mb));
            mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            mb.srcAccessMask =
                VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            g_xpu_vk.vkCmdPipelineBarrier(
                cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
            VkBufferCopy region;
            region.srcOffset = r->view_offset;
            region.dstOffset = 0;
            region.size = bytes;
            g_xpu_vk.vkCmdCopyBuffer(cmd, r->buffer, sb->buffer, 1, &region);
            g_xpu_vk.vkEndCommandBuffer(cmd);
            VkSubmitInfo si;
            memset(&si, 0, sizeof(si));
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            if (g_xpu_vk.vkQueueSubmit(g_xpu_vk.queue, 1, &si, VK_NULL_HANDLE)
                    == VK_SUCCESS) {
                g_xpu_vk.vkQueueWaitIdle(g_xpu_vk.queue);
                copied = 1;
            }
            g_xpu_vk.vkFreeCommandBuffers(g_xpu_vk.device, g_xpu_vk.cmdPool,
                                          1, &cmd);
        }
    }
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
    if (s_readLog) clock_gettime(CLOCK_MONOTONIC, &ts1);
    if (copied && sb->mapped)
        memcpy(dst, sb->mapped, (size_t) bytes);
    else if (r->mapped)
        memcpy(dst, r->mapped, (size_t) bytes);   // slow but correct fallback
    if (s_readLog) {
        clock_gettime(CLOCK_MONOTONIC, &ts2);
        fprintf(stderr, "vk-read: %s %llu B gpu-copy %.1f us memcpy %.1f us "
                "(staging cached=%d)\n",
                copied ? "staged" : "FALLBACK", (unsigned long long) bytes,
                (ts1.tv_sec - ts0.tv_sec) * 1e6 +
                    (ts1.tv_nsec - ts0.tv_nsec) * 1e-3,
                (ts2.tv_sec - ts1.tv_sec) * 1e6 +
                    (ts2.tv_nsec - ts1.tv_nsec) * 1e-3,
                sb ? sb->host_cached : -1);
    }
}

static void cajeta_xpu_vk_free(int64_t handle) {
    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    // A recorded-but-unexecuted dispatch may hold this buffer in a baked
    // descriptor set; destroying it now would fault the flush. Park the
    // handle (slot stays live, so no reuse aliasing) until the batch lands.
    if (g_vk_batch.open) {
        if (g_vk_batch.npendingFree < CAJ_VK_PENDING_FREES) {
            g_vk_batch.pendingFree[g_vk_batch.npendingFree++] = handle;
            pthread_mutex_unlock(&g_xpu_vk_submit_mu);
            return;
        }
        cajeta_xpu_vk_flush();   // backstop: land the batch, then free now
    }
    cajeta_xpu_vk_free_now(handle);
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
}
static void cajeta_xpu_vk_free_now(int64_t handle) {
    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    struct cajeta_vk_buf* r = cajeta_xpu_vk_rec(handle);
    if (r) {
        g_vk_buf_free_hint = (int) (handle - 1);
        if (r->is_view) {
            // A slice view borrows the parent's buffer/memory — clear the slot
            // only; the parent (its owner) destroys the resources.
            r->live = 0; r->mapped = NULL; r->buffer = VK_NULL_HANDLE;
            r->memory = VK_NULL_HANDLE; r->is_view = 0; r->view_offset = 0;
        } else {
            if (r->mapped) g_xpu_vk.vkUnmapMemory(g_xpu_vk.device, r->memory);
            if (r->buffer) g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, r->buffer, NULL);
            if (r->memory) g_xpu_vk.vkFreeMemory(g_xpu_vk.device, r->memory, NULL);
            r->live = 0; r->mapped = NULL; r->buffer = VK_NULL_HANDLE;
            r->memory = VK_NULL_HANDLE;
        }
    }
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
}

// Release a slice VIEW's table slot. Views are the one table entry nothing
// owned: Buffer.slice allocates a slot, the view's KernelBuffer is non-owning, and
// no path ever cleared it — so every slice LEAKED a slot and the per-row 8B
// prefill (~115k slices at ctx512) overflowed any static cap. Guarded on
// is_view so a real (owning or borrowed) handle can never lose its resources
// through this path; freeing a non-view here is a caller bug and is refused.
static void cajeta_xpu_vk_view_release(int64_t handle) {
    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    struct cajeta_vk_buf* r = cajeta_xpu_vk_rec(handle);
    if (r && r->is_view) {
        g_vk_buf_free_hint = (int) (handle - 1);
        r->live = 0; r->mapped = NULL; r->buffer = VK_NULL_HANDLE;
        r->memory = VK_NULL_HANDLE; r->is_view = 0; r->view_offset = 0;
    }
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
}

// --- Vulkan sampled-image (Texture2D) table (Item 8 Stage B) ----------------
// A Texture2D's device handle on Vulkan is a 1-based index into this table. The
// image is R32_SFLOAT (single-channel float, matching the scalar texel), OPTIMAL
// tiled + device-local, used as SAMPLED_IMAGE. Texels are staged through a
// host-visible buffer + copy with layout transitions on upload.
struct cajeta_vk_tex {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    uint32_t w, h, d;       // d = depth: 1 for a 2-D image, >=1 for a 3-D image
    uint32_t layers;        // array-layer count (N for 2D-array, 6 for cube, else 1)
    int layered;            // 1 if the layers are array layers (2D-array/cube) — the
                            // upload copies them via subresource layerCount, not depth
    int live;
    int storage;            // 1 = writable STORAGE_IMAGE (Image2D), 0 = sampled
    int32_t format;         // TextureFormat ordinal (sampled images; storage = R32F)
    VkImageLayout layout;   // current layout (tracked for storage-image barriers)
};
#define CAJETA_VK_MAX_TEXTURES 256
static struct cajeta_vk_tex g_vk_texs[CAJETA_VK_MAX_TEXTURES];
static int g_vk_tex_count;

static struct cajeta_vk_tex* cajeta_xpu_vk_tex_rec(int64_t handle) {
    pthread_mutex_lock(&g_xpu_vk_submit_mu);   // g_vk_texs read (recursive)
    struct cajeta_vk_tex* t = NULL;
    if (handle > 0 && handle <= g_vk_tex_count && g_vk_texs[handle - 1].live)
        t = &g_vk_texs[handle - 1];
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
    return t;
}

// Create a 2-D R32_SFLOAT image + view; return a 1-based table handle (0 on
// failure). `storage`=0 makes a SAMPLED image (Texture2D; contents undefined
// until cajeta_xpu_vk_tex_upload). `storage`=1 makes a writable STORAGE_IMAGE
// (Image2D) usable as an OpImageWrite target and readable back to the host
// (TRANSFER_SRC) — its texels start undefined and are produced by a kernel.
// `imageKind` is the texture kind (1/2/3/4/5 = 1D/2D/3D/2D-array/cube) — the
// single axis selecting the image + view type, the used extent components, and
// whether `arrayLayers` are array layers (2D-array/cube) vs a true depth (3D). A
// 1-D image has h = depth = 1; a 2-D image has depth = 1; a 2D-array/cube has
// depth = 1 and arrayLayers > 1 (cube = 6, with the CUBE_COMPATIBLE flag).
static int64_t cajeta_xpu_vk_tex_alloc(uint32_t w, uint32_t h, int storage,
                                       int32_t format, uint32_t depth, int imageKind,
                                       uint32_t arrayLayers, uint32_t mipLevels) {
    if (w == 0 || h == 0 || depth == 0) return 0;
    if (arrayLayers == 0) arrayLayers = 1;
    int is3d = (imageKind == 3);
    int isCube = (imageKind == 5);
    int layered = (imageKind == 4 || imageKind == 5);   // array layers, not depth
    if (mipLevels == 0) mipLevels = 1;
    // Storage images (Image2D) are R32F only; sampled images (Texture2D) pick a
    // VkFormat from the TextureFormat ordinal. All sample to float in the shader,
    // so the descriptor format is the only thing that varies.
    VkFormat vkfmt = VK_FORMAT_R32_SFLOAT;
    if (!storage) {
        switch (format) {
            case CAJ_TEXFMT_R8_UNORM:    vkfmt = VK_FORMAT_R8_UNORM;            break;
            case CAJ_TEXFMT_RGBA8_UNORM: vkfmt = VK_FORMAT_R8G8B8A8_UNORM;      break;
            case CAJ_TEXFMT_RGBA32F:     vkfmt = VK_FORMAT_R32G32B32A32_SFLOAT; break;
            case CAJ_TEXFMT_R16F:        vkfmt = VK_FORMAT_R16_SFLOAT;          break;
            case CAJ_TEXFMT_RGBA16F:     vkfmt = VK_FORMAT_R16G16B16A16_SFLOAT; break;
            case CAJ_TEXFMT_R32I:        vkfmt = VK_FORMAT_R32_SINT;            break;
            case CAJ_TEXFMT_R32UI:       vkfmt = VK_FORMAT_R32_UINT;            break;
            case CAJ_TEXFMT_RGBA32I:     vkfmt = VK_FORMAT_R32G32B32A32_SINT;   break;
            case CAJ_TEXFMT_RGBA32UI:    vkfmt = VK_FORMAT_R32G32B32A32_UINT;   break;
            case CAJ_TEXFMT_R32F: default: vkfmt = VK_FORMAT_R32_SFLOAT;        break;
        }
    }
    VkImageCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    // 2D-array and cube are both 2-D image types (the layering is in arrayLayers
    // + the view type); only a cube needs the CUBE_COMPATIBLE create flag.
    ici.imageType = is3d ? VK_IMAGE_TYPE_3D
                         : (imageKind == 1 ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D);
    ici.flags = isCube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ici.format = vkfmt;
    ici.extent.width = w; ici.extent.height = h;
    ici.extent.depth = is3d ? depth : 1;
    ici.mipLevels = mipLevels;
    ici.arrayLayers = layered ? arrayLayers : 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = storage
        ? (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
           VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        : (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img = VK_NULL_HANDLE;
    if (g_xpu_vk.vkCreateImage(g_xpu_vk.device, &ici, NULL, &img) != VK_SUCCESS)
        return 0;
    VkMemoryRequirements req;
    memset(&req, 0, sizeof(req));
    g_xpu_vk.vkGetImageMemoryRequirements(g_xpu_vk.device, img, &req);
    int mt = cajeta_xpu_vk_find_memory_type(req.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0)   // no device-local: any type works (host-visible is acceptable)
        mt = cajeta_xpu_vk_find_memory_type(req.memoryTypeBits, 0);
    if (mt < 0) { g_xpu_vk.vkDestroyImage(g_xpu_vk.device, img, NULL); return 0; }
    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t) mt;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (g_xpu_vk.vkAllocateMemory(g_xpu_vk.device, &mai, NULL, &mem)
            != VK_SUCCESS) {
        g_xpu_vk.vkDestroyImage(g_xpu_vk.device, img, NULL); return 0;
    }
    if (g_xpu_vk.vkBindImageMemory(g_xpu_vk.device, img, mem, 0) != VK_SUCCESS) {
        g_xpu_vk.vkFreeMemory(g_xpu_vk.device, mem, NULL);   // L5
        g_xpu_vk.vkDestroyImage(g_xpu_vk.device, img, NULL);
        return 0;
    }
    VkImageViewCreateInfo vci;
    memset(&vci, 0, sizeof(vci));
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = img;
    vci.viewType = is3d ? VK_IMAGE_VIEW_TYPE_3D
                 : (imageKind == 1 ? VK_IMAGE_VIEW_TYPE_1D
                 : (imageKind == 4 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                 : (isCube ? VK_IMAGE_VIEW_TYPE_CUBE
                           : VK_IMAGE_VIEW_TYPE_2D)));
    vci.format = vkfmt;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = mipLevels;
    vci.subresourceRange.layerCount = layered ? arrayLayers : 1;
    VkImageView view = VK_NULL_HANDLE;
    if (g_xpu_vk.vkCreateImageView(g_xpu_vk.device, &vci, NULL, &view)
            != VK_SUCCESS) {
        g_xpu_vk.vkFreeMemory(g_xpu_vk.device, mem, NULL);
        g_xpu_vk.vkDestroyImage(g_xpu_vk.device, img, NULL);
        return 0;
    }
    pthread_mutex_lock(&g_xpu_vk_submit_mu);   // g_vk_texs table RMW
    int slot = -1;
    for (int i = 0; i < g_vk_tex_count; ++i)
        if (!g_vk_texs[i].live) { slot = i; break; }
    if (slot < 0) {
        if (g_vk_tex_count >= CAJETA_VK_MAX_TEXTURES) {
            pthread_mutex_unlock(&g_xpu_vk_submit_mu);
            g_xpu_vk.vkDestroyImageView(g_xpu_vk.device, view, NULL);
            g_xpu_vk.vkFreeMemory(g_xpu_vk.device, mem, NULL);
            g_xpu_vk.vkDestroyImage(g_xpu_vk.device, img, NULL);
            return 0;
        }
        slot = g_vk_tex_count++;
    }
    g_vk_texs[slot].image = img;
    g_vk_texs[slot].memory = mem;
    g_vk_texs[slot].view = view;
    g_vk_texs[slot].w = w; g_vk_texs[slot].h = h;
    g_vk_texs[slot].d = is3d ? depth : 1;
    g_vk_texs[slot].layers = layered ? arrayLayers : 1;
    g_vk_texs[slot].layered = layered;
    g_vk_texs[slot].live = 1;
    g_vk_texs[slot].storage = storage;
    g_vk_texs[slot].format = storage ? CAJ_TEXFMT_R32F : format;
    g_vk_texs[slot].layout = VK_IMAGE_LAYOUT_UNDEFINED;
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
    return (int64_t) (slot + 1);
}

// Copy `src` (lw*lh*ld*layers row-major float32 texels) into mip `level` of image
// `t`, leaving that subresource in SHADER_READ_ONLY_OPTIMAL ready to sample.
// `layers` is the array-layer count (1 for a plain 2-D/3-D image; N for a 2-D
// array; 6 for a cube) — the layers are laid out contiguously after the (lw*lh*ld)
// plane, copied via the subresource layerCount (NOT extent.depth). Transient
// host-visible staging buffer + a one-time command buffer (barrier / copy /
// barrier) on the shared VkQueue (this routine takes g_xpu_vk_submit_mu itself).
// Returns 1 on success, 0 if the command buffer couldn't be recorded/submitted
// (the subresource is left uninitialized). The per-level barriers use
// baseMipLevel=level/levelCount=1, so each level is transitioned independently —
// uploading every level of a mip chain leaves the whole image SHADER_READ.
static int cajeta_xpu_vk_tex_copy_region(struct cajeta_vk_tex* t, const float* src,
                                         uint32_t lw, uint32_t lh, uint32_t ld,
                                         uint32_t layers, uint32_t level,
                                         int32_t format) {
    if (!t || !src || lw == 0 || lh == 0 || ld == 0) return 0;
    if (layers == 0) layers = 1;
    size_t texels =
        (size_t) lw * lh * ld * layers * cajeta_texfmt_channels(format);
    uint64_t bytes =
        (uint64_t) lw * lh * ld * layers * cajeta_texfmt_texel_bytes(format);
    int64_t staging = cajeta_xpu_vk_alloc(bytes);   // host-visible+coherent
    if (!staging) return 0;
    void* m = cajeta_xpu_vk_mapped(staging);
    if (m) cajeta_texfmt_encode(m, src, texels, format);   // float memcpy / UNORM quantize
    struct cajeta_vk_buf* sb = cajeta_xpu_vk_rec(staging);

    // The staging copy submits to the shared VkQueue/VkCommandPool — serialize it
    // against concurrent launches / AS builds (same external-sync requirement).
    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    VkCommandBufferAllocateInfo cbai;
    memset(&cbai, 0, sizeof(cbai));
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = g_xpu_vk.cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    int ok = 0;
    if (g_xpu_vk.vkAllocateCommandBuffers(g_xpu_vk.device, &cbai, &cmd)
            == VK_SUCCESS && sb) {
        ok = 1;
        VkCommandBufferBeginInfo cbbi;
        memset(&cbbi, 0, sizeof(cbbi));
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        g_xpu_vk.vkBeginCommandBuffer(cmd, &cbbi);

        VkImageMemoryBarrier toDst;
        memset(&toDst, 0, sizeof(toDst));
        toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = t->image;
        toDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toDst.subresourceRange.baseMipLevel = level;
        toDst.subresourceRange.levelCount = 1;
        toDst.subresourceRange.layerCount = layers;
        toDst.srcAccessMask = 0;
        toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        g_xpu_vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                      VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                      0, NULL, 0, NULL, 1, &toDst);

        VkBufferImageCopy region;
        memset(&region, 0, sizeof(region));
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = level;
        region.imageSubresource.layerCount = layers;
        region.imageExtent.width = lw;
        region.imageExtent.height = lh;
        region.imageExtent.depth = ld;
        g_xpu_vk.vkCmdCopyBufferToImage(cmd, sb->buffer, t->image,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        1, &region);

        VkImageMemoryBarrier toRead = toDst;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        g_xpu_vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                      0, NULL, 0, NULL, 1, &toRead);

        g_xpu_vk.vkEndCommandBuffer(cmd);
        VkSubmitInfo si;
        memset(&si, 0, sizeof(si));
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        g_xpu_vk.vkQueueSubmit(g_xpu_vk.queue, 1, &si, VK_NULL_HANDLE);
        g_xpu_vk.vkQueueWaitIdle(g_xpu_vk.queue);
        g_xpu_vk.vkFreeCommandBuffers(g_xpu_vk.device, g_xpu_vk.cmdPool, 1, &cmd);
    }
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
    cajeta_xpu_vk_free(staging);
    return ok;
}

// Stage `data` (w*h row-major float32 texels) into mip level 0, leaving it in
// SHADER_READ_ONLY_OPTIMAL ready to sample.
static void cajeta_xpu_vk_tex_upload(int64_t handle, const float* src,
                                     uint32_t w, uint32_t h, int32_t format) {
    struct cajeta_vk_tex* t = cajeta_xpu_vk_tex_rec(handle);
    if (!t || !src || w != t->w || h != t->h) return;
    // A layered image (2-D array / cube) carries its planes in array layers, not
    // depth: copy depth 1 × N layers. A 3-D image carries them in depth × 1 layer.
    uint32_t dd = t->layered ? 1 : (t->d ? t->d : 1);
    uint32_t ll = t->layered ? (t->layers ? t->layers : 1) : 1;
    // M7: if the upload couldn't be recorded the image stays UNDEFINED but a
    // later launch binds it as SHADER_READ_ONLY_OPTIMAL — surface, don't fail
    // silently.
    if (!cajeta_xpu_vk_tex_copy_region(t, src, w, h, dd, ll, 0, format))
        fprintf(stderr, "cajeta.xpu: texture upload could not record/submit "
                "(handle %lld); the image is left uninitialized\n",
                (long long) handle);
}

// Stage one mip level: `src` is lw*lh row-major float32 texels for `level`
// (depth 1 — mip Texture2D only). Each level is an independent copy_region.
static void cajeta_xpu_vk_tex_upload_level(int64_t handle, const float* src,
                                           uint32_t lw, uint32_t lh,
                                           uint32_t level, int32_t format) {
    struct cajeta_vk_tex* t = cajeta_xpu_vk_tex_rec(handle);
    if (!t || !src) return;
    if (!cajeta_xpu_vk_tex_copy_region(t, src, lw, lh, 1, 1, level, format))
        fprintf(stderr, "cajeta.xpu: texture mip-level upload could not "
                "record/submit (handle %lld, level %u)\n",
                (long long) handle, level);
}

static void cajeta_xpu_vk_tex_free(int64_t handle) {
    pthread_mutex_lock(&g_xpu_vk_submit_mu);   // serialize vs launch + table
    struct cajeta_vk_tex* t = cajeta_xpu_vk_tex_rec(handle);
    if (t) {
        if (t->view) g_xpu_vk.vkDestroyImageView(g_xpu_vk.device, t->view, NULL);
        if (t->image) g_xpu_vk.vkDestroyImage(g_xpu_vk.device, t->image, NULL);
        if (t->memory) g_xpu_vk.vkFreeMemory(g_xpu_vk.device, t->memory, NULL);
        t->live = 0; t->image = VK_NULL_HANDLE; t->view = VK_NULL_HANDLE;
        t->memory = VK_NULL_HANDLE;
    }
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
}

// Read a storage image (Image2D) back to host memory: w*h row-major float32
// texels into `data`. After a kernel's OpImageWrite the image is in GENERAL
// layout; transition it to TRANSFER_SRC, copy to a host-visible staging buffer,
// and memcpy out. Mirrors cajeta_xpu_vk_tex_upload in reverse (one-time command
// buffer, serialized on the shared queue). The image is left in TRANSFER_SRC
// (its tracked layout is updated, so a subsequent dispatch re-barriers to GENERAL).
static void cajeta_xpu_vk_tex_download(int64_t handle, void* data,
                                       uint32_t w, uint32_t h) {
    struct cajeta_vk_tex* t = cajeta_xpu_vk_tex_rec(handle);
    if (!t || !data || w != t->w || h != t->h) return;
    uint64_t bytes = (uint64_t) w * h * sizeof(float);
    int64_t staging = caj_vk_alloc_pref(bytes, /*forStaging=*/1); // cached
    if (!staging) return;
    struct cajeta_vk_buf* sb = cajeta_xpu_vk_rec(staging);

    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    VkCommandBufferAllocateInfo cbai;
    memset(&cbai, 0, sizeof(cbai));
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = g_xpu_vk.cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    int copied = 0;
    if (g_xpu_vk.vkAllocateCommandBuffers(g_xpu_vk.device, &cbai, &cmd)
            == VK_SUCCESS && sb) {
        copied = 1;
        VkCommandBufferBeginInfo cbbi;
        memset(&cbbi, 0, sizeof(cbbi));
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        g_xpu_vk.vkBeginCommandBuffer(cmd, &cbbi);

        VkImageMemoryBarrier toSrc;
        memset(&toSrc, 0, sizeof(toSrc));
        toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toSrc.oldLayout = t->layout;   // GENERAL after a write (or UNDEFINED)
        toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.image = t->image;
        toSrc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toSrc.subresourceRange.levelCount = 1;
        toSrc.subresourceRange.layerCount = 1;
        toSrc.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        g_xpu_vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                      VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                      0, NULL, 0, NULL, 1, &toSrc);

        VkBufferImageCopy region;
        memset(&region, 0, sizeof(region));
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = w;
        region.imageExtent.height = h;
        region.imageExtent.depth = 1;
        g_xpu_vk.vkCmdCopyImageToBuffer(cmd, t->image,
                                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                        sb->buffer, 1, &region);

        g_xpu_vk.vkEndCommandBuffer(cmd);
        VkSubmitInfo si;
        memset(&si, 0, sizeof(si));
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        g_xpu_vk.vkQueueSubmit(g_xpu_vk.queue, 1, &si, VK_NULL_HANDLE);
        g_xpu_vk.vkQueueWaitIdle(g_xpu_vk.queue);
        g_xpu_vk.vkFreeCommandBuffers(g_xpu_vk.device, g_xpu_vk.cmdPool, 1, &cmd);
        t->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
    if (copied) {
        void* m = cajeta_xpu_vk_mapped(staging);
        if (m) memcpy(data, m, (size_t) bytes);   // host-coherent: no flush
    }
    cajeta_xpu_vk_free(staging);
}

// Create a transient VkSampler from a cajeta Sampler's modes: filterMode 0 =
// nearest, 1 = linear; addressMode 0 = clamp-to-edge, 1 = repeat. Normalized
// coords (unnormalizedCoordinates = FALSE); single mip (sample at LOD 0).
// Returns the VkSampler as an int64 (0 on failure) so the build-shared launch
// translation never names a Vulkan type. Pair with cajeta_xpu_vk_destroy_sampler.
static int64_t cajeta_xpu_vk_make_sampler(int32_t filterMode,
                                          int32_t addressMode) {
    VkFilter f = filterMode == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    VkSamplerAddressMode a = addressMode == 1
                                 ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                 : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSamplerCreateInfo sci;
    memset(&sci, 0, sizeof(sci));
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = f; sci.minFilter = f;
    sci.mipmapMode = filterMode == 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                     : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = a; sci.addressModeV = a; sci.addressModeW = a;
    // maxLod must admit the highest mip level an explicit-LOD sample can request;
    // 0.0 clamps every LOD to level 0 (so sampleLod(.., lod>0) never reaches the
    // smaller mips). VK_LOD_CLAMP_NONE (1000.0) imposes no clamp — single-level
    // (non-mip) Texture2D is unaffected (only level 0 exists to sample).
    sci.minLod = 0.0f; sci.maxLod = VK_LOD_CLAMP_NONE;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    sci.unnormalizedCoordinates = VK_FALSE;
    VkSampler s = VK_NULL_HANDLE;
    if (g_xpu_vk.vkCreateSampler(g_xpu_vk.device, &sci, NULL, &s) != VK_SUCCESS)
        return 0;
    return (int64_t) (uintptr_t) s;
}

static void cajeta_xpu_vk_destroy_sampler(int64_t handle) {
    if (!handle) return;
    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    // Same deferral as vk_free: a recorded dispatch may reference it.
    if (g_vk_batch.open
            && g_vk_batch.npendingSampler < CAJ_VK_PENDING_SAMPLERS) {
        g_vk_batch.pendingSampler[g_vk_batch.npendingSampler++] = handle;
        pthread_mutex_unlock(&g_xpu_vk_submit_mu);
        return;
    }
    g_xpu_vk.vkDestroySampler(g_xpu_vk.device,
                              (VkSampler) (uintptr_t) handle, NULL);
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
}

// --- Vulkan acceleration-structure (BVH) table (Part C inc 3b) ---------------
// An AccelerationStructure's device handle on Vulkan is a 1-based index into
// this table. v1 builds a single bottom-level AS over AABB (procedural) geometry
// — the spatial-index primitive the RayQuery walks. All build inputs/scratch are
// device-address buffers (VK_KHR_buffer_device_address); the AS itself is bound
// in a kernel as VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR (see the launch
// path). Only reached when g_xpu_vk.rayQuery == 1.
struct cajeta_vk_accel {
    // `accel` is the TOP-LEVEL AS — the only thing a ray-query descriptor may bind
    // (VUID-VkWriteDescriptorSetAccelerationStructureKHR-pAccelerationStructures-03579).
    // It instances the bottom-level AS, which must outlive it, so we own both here.
    VkAccelerationStructureKHR accel;   // TLAS (bound by the descriptor)
    VkBuffer asBuf;                     // TLAS backing store (must outlive the AS)
    VkDeviceMemory asMem;
    VkAccelerationStructureKHR blas;    // BLAS the TLAS references (must outlive it)
    VkBuffer blasBuf;
    VkDeviceMemory blasMem;
    int live;
};
#define CAJETA_VK_MAX_ACCELS 256
static struct cajeta_vk_accel g_vk_accels[CAJETA_VK_MAX_ACCELS];
static int g_vk_accel_count;

static struct cajeta_vk_accel* cajeta_xpu_vk_accel_rec(int64_t handle) {
    if (handle <= 0 || handle > g_vk_accel_count) return NULL;
    struct cajeta_vk_accel* a = &g_vk_accels[handle - 1];
    return a->live ? a : NULL;
}

// Create a buffer that exposes a device address (VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
// + SHADER_DEVICE_ADDRESS usage), backed by memory satisfying `props`. Used for the
// AS build input/scratch/store. Returns 1 on success. `outMapped` non-NULL means the
// caller will fill the buffer from the host, so `props` MUST be host-visible (no
// fallback — a non-mappable type would break the memcpy). For a device-only buffer
// (`outMapped` NULL), if `props` (e.g. DEVICE_LOCAL) isn't available we fall back to
// any memory type — correctness over the device-local perf preference.
static int cajeta_xpu_vk_make_addr_buffer(uint64_t bytes, VkBufferUsageFlags usage,
                                          VkMemoryPropertyFlags props,
                                          VkBuffer* outBuf, VkDeviceMemory* outMem,
                                          void** outMapped) {
    if (bytes == 0) return 0;
    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf = VK_NULL_HANDLE;
    if (g_xpu_vk.vkCreateBuffer(g_xpu_vk.device, &bci, NULL, &buf) != VK_SUCCESS)
        return 0;
    VkMemoryRequirements req;
    memset(&req, 0, sizeof(req));
    g_xpu_vk.vkGetBufferMemoryRequirements(g_xpu_vk.device, buf, &req);
    int mt = cajeta_xpu_vk_find_memory_type(req.memoryTypeBits, props);
    if (mt < 0 && !outMapped)   // device-only buffer: any memory type is fine
        mt = cajeta_xpu_vk_find_memory_type(req.memoryTypeBits, 0);
    if (mt < 0) { g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, buf, NULL); return 0; }
    VkMemoryAllocateFlagsInfo fi;
    memset(&fi, 0, sizeof(fi));
    fi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &fi;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t) mt;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (g_xpu_vk.vkAllocateMemory(g_xpu_vk.device, &mai, NULL, &mem) != VK_SUCCESS) {
        g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, buf, NULL);
        return 0;
    }
    if (g_xpu_vk.vkBindBufferMemory(g_xpu_vk.device, buf, mem, 0) != VK_SUCCESS) {
        g_xpu_vk.vkFreeMemory(g_xpu_vk.device, mem, NULL);   // L5
        g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, buf, NULL);
        return 0;
    }
    if (outMapped) {
        *outMapped = NULL;
        if (g_xpu_vk.vkMapMemory(g_xpu_vk.device, mem, 0, VK_WHOLE_SIZE, 0,
                                 outMapped) != VK_SUCCESS) {
            g_xpu_vk.vkFreeMemory(g_xpu_vk.device, mem, NULL);
            g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, buf, NULL);
            return 0;
        }
    }
    *outBuf = buf;
    *outMem = mem;
    return 1;
}

static VkDeviceAddress cajeta_xpu_vk_buf_addr(VkBuffer b) {
    VkBufferDeviceAddressInfo i;
    memset(&i, 0, sizeof(i));
    i.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    i.buffer = b;
    return g_xpu_vk.vkGetBufferDeviceAddress(g_xpu_vk.device, &i);
}

// Build a ray-traceable scene over `count` AABBs, each packed as 6 float32
// (minX,minY,minZ,maxX,maxY,maxZ) — byte-identical to VkAabbPositionsKHR, so the
// cajeta float32[] uploads straight in. A ray-query descriptor can ONLY bind a
// TOP-LEVEL AS (VUID-...-03579), so we build a bottom-level AS over the AABBs and
// then a top-level AS with one identity-transform instance referencing it. Both
// survive in the table (the TLAS references the BLAS by device address); the AABB
// input, instance buffer, and scratch are transient and freed before returning.
// (Binding the BLAS directly happens to traverse on AMD but yields zero hits on
// NVIDIA — caught by the validation layer.) Returns a 1-based table handle (0 on
// failure / no RT device).
static int64_t cajeta_xpu_vk_accel_build_aabbs(const float* aabbs, uint32_t count) {
    if (!g_xpu_vk.rayQuery || !aabbs || count == 0) return 0;

    // Serialize the whole build: it submits to the shared queue/cmdpool and
    // mutates the g_vk_accels table (slot-find + count++), both of which race
    // a concurrent launch/build/free from another OS thread without this.
    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    // Transient build inputs/scratch (freed at accel_done).
    VkBuffer aabbBuf = VK_NULL_HANDLE, instBuf = VK_NULL_HANDLE,
             blScratch = VK_NULL_HANDLE, tlScratch = VK_NULL_HANDLE;
    VkDeviceMemory aabbMem = VK_NULL_HANDLE, instMem = VK_NULL_HANDLE,
                   blScratchMem = VK_NULL_HANDLE, tlScratchMem = VK_NULL_HANDLE;
    void* aabbMapped = NULL; void* instMapped = NULL;
    // BLAS + TLAS (survive on success; torn down on failure).
    VkBuffer blasBuf = VK_NULL_HANDLE, tlasBuf = VK_NULL_HANDLE;
    VkDeviceMemory blasMem = VK_NULL_HANDLE, tlasMem = VK_NULL_HANDLE;
    VkAccelerationStructureKHR blas = VK_NULL_HANDLE, tlas = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    int64_t result = 0;
    // Scratch device addresses must be aligned to minAccelerationStructureScratch
    // OffsetAlignment (VUID-...-scratchData-03710); over-allocate by (align-1) and
    // round the address up. A power of two (Vulkan requires it).
    VkDeviceSize scratchAlign = g_xpu_vk.scratchAlign ? g_xpu_vk.scratchAlign : 256;

    // ===== Bottom-level AS over the AABBs =====
    uint64_t aabbBytes = (uint64_t) count * sizeof(VkAabbPositionsKHR);
    if (!cajeta_xpu_vk_make_addr_buffer(
            aabbBytes,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &aabbBuf, &aabbMem, &aabbMapped))
        goto accel_done;
    memcpy(aabbMapped, aabbs, (size_t) aabbBytes);

    VkAccelerationStructureGeometryKHR blGeom;
    memset(&blGeom, 0, sizeof(blGeom));
    blGeom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    blGeom.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
    blGeom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    blGeom.geometry.aabbs.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
    blGeom.geometry.aabbs.stride = sizeof(VkAabbPositionsKHR);
    blGeom.geometry.aabbs.data.deviceAddress = cajeta_xpu_vk_buf_addr(aabbBuf);

    VkAccelerationStructureBuildGeometryInfoKHR blBgi;
    memset(&blBgi, 0, sizeof(blBgi));
    blBgi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    blBgi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    blBgi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    blBgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    blBgi.geometryCount = 1;
    blBgi.pGeometries = &blGeom;

    uint32_t blPrim = count;
    VkAccelerationStructureBuildSizesInfoKHR blSizes;
    memset(&blSizes, 0, sizeof(blSizes));
    blSizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    g_xpu_vk.vkGetAccelerationStructureBuildSizesKHR(
        g_xpu_vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &blBgi,
        &blPrim, &blSizes);
    if (blSizes.accelerationStructureSize == 0 || blSizes.buildScratchSize == 0)
        goto accel_done;

    if (!cajeta_xpu_vk_make_addr_buffer(
            blSizes.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &blasBuf, &blasMem, NULL))
        goto accel_done;
    VkAccelerationStructureCreateInfoKHR blAci;
    memset(&blAci, 0, sizeof(blAci));
    blAci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    blAci.buffer = blasBuf;
    blAci.size = blSizes.accelerationStructureSize;
    blAci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    if (g_xpu_vk.vkCreateAccelerationStructureKHR(g_xpu_vk.device, &blAci, NULL,
                                                  &blas) != VK_SUCCESS)
        goto accel_done;

    if (!cajeta_xpu_vk_make_addr_buffer(blSizes.buildScratchSize + scratchAlign - 1,
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                        &blScratch, &blScratchMem, NULL))
        goto accel_done;
    blBgi.dstAccelerationStructure = blas;
    {
        VkDeviceAddress s = cajeta_xpu_vk_buf_addr(blScratch);
        s = (s + scratchAlign - 1) & ~((VkDeviceAddress) scratchAlign - 1);
        blBgi.scratchData.deviceAddress = s;
    }
    VkAccelerationStructureBuildRangeInfoKHR blRange;
    memset(&blRange, 0, sizeof(blRange));
    blRange.primitiveCount = count;
    const VkAccelerationStructureBuildRangeInfoKHR* blRanges = &blRange;

    // ===== Top-level AS over one instance referencing the BLAS =====
    VkAccelerationStructureDeviceAddressInfoKHR blAddrInfo;
    memset(&blAddrInfo, 0, sizeof(blAddrInfo));
    blAddrInfo.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    blAddrInfo.accelerationStructure = blas;   // valid once the AS object exists
    VkDeviceAddress blasAddr =
        g_xpu_vk.vkGetAccelerationStructureDeviceAddressKHR(g_xpu_vk.device,
                                                            &blAddrInfo);

    VkAccelerationStructureInstanceKHR inst;
    memset(&inst, 0, sizeof(inst));
    inst.transform.matrix[0][0] = 1.0f;        // identity 3x4 (row-major)
    inst.transform.matrix[1][1] = 1.0f;
    inst.transform.matrix[2][2] = 1.0f;
    inst.mask = 0xFF;                          // matches the kernel's cullMask
    inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    inst.accelerationStructureReference = (uint64_t) blasAddr;

    if (!cajeta_xpu_vk_make_addr_buffer(
            sizeof(inst),
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &instBuf, &instMem, &instMapped))
        goto accel_done;
    memcpy(instMapped, &inst, sizeof(inst));

    VkAccelerationStructureGeometryKHR tlGeom;
    memset(&tlGeom, 0, sizeof(tlGeom));
    tlGeom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    tlGeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlGeom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    tlGeom.geometry.instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tlGeom.geometry.instances.arrayOfPointers = VK_FALSE;
    tlGeom.geometry.instances.data.deviceAddress = cajeta_xpu_vk_buf_addr(instBuf);

    VkAccelerationStructureBuildGeometryInfoKHR tlBgi;
    memset(&tlBgi, 0, sizeof(tlBgi));
    tlBgi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    tlBgi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlBgi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tlBgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlBgi.geometryCount = 1;
    tlBgi.pGeometries = &tlGeom;

    uint32_t tlPrim = 1;
    VkAccelerationStructureBuildSizesInfoKHR tlSizes;
    memset(&tlSizes, 0, sizeof(tlSizes));
    tlSizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    g_xpu_vk.vkGetAccelerationStructureBuildSizesKHR(
        g_xpu_vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlBgi,
        &tlPrim, &tlSizes);
    if (tlSizes.accelerationStructureSize == 0 || tlSizes.buildScratchSize == 0)
        goto accel_done;

    if (!cajeta_xpu_vk_make_addr_buffer(
            tlSizes.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &tlasBuf, &tlasMem, NULL))
        goto accel_done;
    VkAccelerationStructureCreateInfoKHR tlAci;
    memset(&tlAci, 0, sizeof(tlAci));
    tlAci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    tlAci.buffer = tlasBuf;
    tlAci.size = tlSizes.accelerationStructureSize;
    tlAci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    if (g_xpu_vk.vkCreateAccelerationStructureKHR(g_xpu_vk.device, &tlAci, NULL,
                                                  &tlas) != VK_SUCCESS)
        goto accel_done;

    if (!cajeta_xpu_vk_make_addr_buffer(tlSizes.buildScratchSize + scratchAlign - 1,
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                        &tlScratch, &tlScratchMem, NULL))
        goto accel_done;
    tlBgi.dstAccelerationStructure = tlas;
    {
        VkDeviceAddress s = cajeta_xpu_vk_buf_addr(tlScratch);
        s = (s + scratchAlign - 1) & ~((VkDeviceAddress) scratchAlign - 1);
        tlBgi.scratchData.deviceAddress = s;
    }
    VkAccelerationStructureBuildRangeInfoKHR tlRange;
    memset(&tlRange, 0, sizeof(tlRange));
    tlRange.primitiveCount = 1;
    const VkAccelerationStructureBuildRangeInfoKHR* tlRanges = &tlRange;

    // ===== Record both builds in one command buffer: BLAS, barrier, TLAS =====
    VkCommandBufferAllocateInfo cbai;
    memset(&cbai, 0, sizeof(cbai));
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = g_xpu_vk.cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (g_xpu_vk.vkAllocateCommandBuffers(g_xpu_vk.device, &cbai, &cmd)
            != VK_SUCCESS)
        goto accel_done;
    VkCommandBufferBeginInfo cbbi;
    memset(&cbbi, 0, sizeof(cbbi));
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g_xpu_vk.vkBeginCommandBuffer(cmd, &cbbi);
    g_xpu_vk.vkCmdBuildAccelerationStructuresKHR(cmd, 1, &blBgi, &blRanges);
    // BLAS write -> TLAS-build read: the TLAS build reads the just-built BLAS.
    {
        VkMemoryBarrier b;
        memset(&b, 0, sizeof(b));
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        b.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        g_xpu_vk.vkCmdPipelineBarrier(
            cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &b, 0,
            NULL, 0, NULL);
    }
    g_xpu_vk.vkCmdBuildAccelerationStructuresKHR(cmd, 1, &tlBgi, &tlRanges);
    // TLAS write -> ray-query read. vkQueueWaitIdle below guarantees the builds
    // EXECUTED, but the memory model still needs this availability/visibility
    // dependency before the read. dst stage is COMPUTE because ray *query* (vs. a
    // ray-tracing pipeline) runs in the compute shader.
    {
        VkMemoryBarrier b;
        memset(&b, 0, sizeof(b));
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        b.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        g_xpu_vk.vkCmdPipelineBarrier(
            cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0, NULL, 0, NULL);
    }
    g_xpu_vk.vkEndCommandBuffer(cmd);
    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (g_xpu_vk.vkQueueSubmit(g_xpu_vk.queue, 1, &si, VK_NULL_HANDLE)
            != VK_SUCCESS)
        goto accel_done;
    g_xpu_vk.vkQueueWaitIdle(g_xpu_vk.queue);

    // Record in the table (TLAS bound by the descriptor; BLAS kept alive). Clear
    // the survivors so the cleanup below doesn't tear them down.
    {
        int slot = -1;
        for (int i = 0; i < g_vk_accel_count; ++i)
            if (!g_vk_accels[i].live) { slot = i; break; }
        if (slot < 0) {
            if (g_vk_accel_count >= CAJETA_VK_MAX_ACCELS) goto accel_done;
            slot = g_vk_accel_count++;
        }
        g_vk_accels[slot].accel = tlas;
        g_vk_accels[slot].asBuf = tlasBuf;
        g_vk_accels[slot].asMem = tlasMem;
        g_vk_accels[slot].blas = blas;
        g_vk_accels[slot].blasBuf = blasBuf;
        g_vk_accels[slot].blasMem = blasMem;
        g_vk_accels[slot].live = 1;
        result = (int64_t) (slot + 1);
        tlas = VK_NULL_HANDLE; tlasBuf = VK_NULL_HANDLE; tlasMem = VK_NULL_HANDLE;
        blas = VK_NULL_HANDLE; blasBuf = VK_NULL_HANDLE; blasMem = VK_NULL_HANDLE;
    }

accel_done:
    if (cmd) g_xpu_vk.vkFreeCommandBuffers(g_xpu_vk.device, g_xpu_vk.cmdPool, 1,
                                           &cmd);
    if (blScratch) g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, blScratch, NULL);
    if (blScratchMem) g_xpu_vk.vkFreeMemory(g_xpu_vk.device, blScratchMem, NULL);
    if (tlScratch) g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, tlScratch, NULL);
    if (tlScratchMem) g_xpu_vk.vkFreeMemory(g_xpu_vk.device, tlScratchMem, NULL);
    if (aabbMapped) g_xpu_vk.vkUnmapMemory(g_xpu_vk.device, aabbMem);
    if (aabbBuf) g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, aabbBuf, NULL);
    if (aabbMem) g_xpu_vk.vkFreeMemory(g_xpu_vk.device, aabbMem, NULL);
    if (instMapped) g_xpu_vk.vkUnmapMemory(g_xpu_vk.device, instMem);
    if (instBuf) g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, instBuf, NULL);
    if (instMem) g_xpu_vk.vkFreeMemory(g_xpu_vk.device, instMem, NULL);
    // On failure these survive (success cleared them); tear them down.
    if (tlas) g_xpu_vk.vkDestroyAccelerationStructureKHR(g_xpu_vk.device, tlas, NULL);
    if (tlasBuf) g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, tlasBuf, NULL);
    if (tlasMem) g_xpu_vk.vkFreeMemory(g_xpu_vk.device, tlasMem, NULL);
    if (blas) g_xpu_vk.vkDestroyAccelerationStructureKHR(g_xpu_vk.device, blas, NULL);
    if (blasBuf) g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, blasBuf, NULL);
    if (blasMem) g_xpu_vk.vkFreeMemory(g_xpu_vk.device, blasMem, NULL);
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
    return result;
}

// Triangle BLAS twin of cajeta_xpu_vk_accel_build_aabbs: a bottom-level AS over
// `triCount` triangles from a vertex soup (`stride` floats per vertex; 3 = tight).
// Non-indexed (VK_INDEX_TYPE_NONE_KHR): vertexCount = triCount*3, primCount =
// triCount. The vertex buffer is a transient build input (freed after the build);
// only the AS backing store survives, exactly like the AABB path.
static int64_t cajeta_xpu_vk_accel_build_triangles(const float* verts,
                                                   uint32_t triCount,
                                                   uint32_t stride) {
    if (!g_xpu_vk.rayQuery || !verts || triCount == 0 || stride < 3u) return 0;

    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    VkBuffer vbuf = VK_NULL_HANDLE, asBuf = VK_NULL_HANDLE,
             scratchBuf = VK_NULL_HANDLE;
    VkDeviceMemory vmem = VK_NULL_HANDLE, asMem = VK_NULL_HANDLE,
                   scratchMem = VK_NULL_HANDLE;
    VkAccelerationStructureKHR accel = VK_NULL_HANDLE;   // the BLAS
    // TLAS over one instance referencing the BLAS. Ray query traces the TOP-LEVEL
    // AS — a BLAS alone is NOT traceable (NVIDIA returns no hits; RADV happened to
    // tolerate it, which is why the BLAS-only path passed there). Built in a
    // second submit once the BLAS is complete (mirrors the AABB path's TLAS).
    VkBuffer instBuf = VK_NULL_HANDLE, tlasBuf = VK_NULL_HANDLE,
             tlScratch = VK_NULL_HANDLE;
    VkDeviceMemory instMem = VK_NULL_HANDLE, tlasMem = VK_NULL_HANDLE,
                   tlScratchMem = VK_NULL_HANDLE;
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    void* instMapped = NULL;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    int64_t result = 0;

    // 1. Vertex input buffer (triCount*3 vertices, `stride` floats each).
    uint32_t vertexCount = triCount * 3u;
    uint64_t vBytes = (uint64_t) vertexCount * stride * sizeof(float);
    void* vMapped = NULL;
    if (!cajeta_xpu_vk_make_addr_buffer(
            vBytes,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &vbuf, &vmem, &vMapped))
        goto tri_done;
    memcpy(vMapped, verts, (size_t) vBytes);

    // 2. Triangle geometry descriptor.
    VkAccelerationStructureGeometryKHR geom;
    memset(&geom, 0, sizeof(geom));
    geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    // Non-opaque: the ray-query `proceed()` loop ENUMERATES each triangle hit as a
    // candidate (candidateType 0), matching the software walk's enumerate-all model
    // (the software path has no commit yet — confirm/generate is inc 3). Opaque
    // triangles would auto-commit and never surface as candidates in the loop.
    geom.flags = 0;
    geom.geometry.triangles.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geom.geometry.triangles.vertexData.deviceAddress = cajeta_xpu_vk_buf_addr(vbuf);
    geom.geometry.triangles.vertexStride = (VkDeviceSize) stride * sizeof(float);
    geom.geometry.triangles.maxVertex = vertexCount - 1u;
    geom.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;

    // 3. Sizes.
    VkAccelerationStructureBuildGeometryInfoKHR bgi;
    memset(&bgi, 0, sizeof(bgi));
    bgi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    bgi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bgi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bgi.geometryCount = 1;
    bgi.pGeometries = &geom;

    uint32_t primCount = triCount;
    VkAccelerationStructureBuildSizesInfoKHR sizes;
    memset(&sizes, 0, sizeof(sizes));
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    g_xpu_vk.vkGetAccelerationStructureBuildSizesKHR(
        g_xpu_vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bgi,
        &primCount, &sizes);
    if (sizes.accelerationStructureSize == 0 || sizes.buildScratchSize == 0)
        goto tri_done;

    // 4. AS backing store + object.
    if (!cajeta_xpu_vk_make_addr_buffer(
            sizes.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &asBuf, &asMem, NULL))
        goto tri_done;
    VkAccelerationStructureCreateInfoKHR aci;
    memset(&aci, 0, sizeof(aci));
    aci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    aci.buffer = asBuf;
    aci.size = sizes.accelerationStructureSize;
    aci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    if (g_xpu_vk.vkCreateAccelerationStructureKHR(g_xpu_vk.device, &aci, NULL,
                                                  &accel) != VK_SUCCESS)
        goto tri_done;

    // 5. Scratch (aligned).
    VkDeviceSize scratchAlign = g_xpu_vk.scratchAlign ? g_xpu_vk.scratchAlign : 256;
    if (!cajeta_xpu_vk_make_addr_buffer(sizes.buildScratchSize + scratchAlign - 1,
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                        &scratchBuf, &scratchMem, NULL))
        goto tri_done;
    bgi.dstAccelerationStructure = accel;
    {
        VkDeviceAddress sAddr = cajeta_xpu_vk_buf_addr(scratchBuf);
        sAddr = (sAddr + scratchAlign - 1) & ~((VkDeviceAddress) scratchAlign - 1);
        bgi.scratchData.deviceAddress = sAddr;
    }

    // 6. Record + submit.
    VkAccelerationStructureBuildRangeInfoKHR range;
    memset(&range, 0, sizeof(range));
    range.primitiveCount = triCount;
    const VkAccelerationStructureBuildRangeInfoKHR* pranges = &range;

    VkCommandBufferAllocateInfo cbai;
    memset(&cbai, 0, sizeof(cbai));
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = g_xpu_vk.cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (g_xpu_vk.vkAllocateCommandBuffers(g_xpu_vk.device, &cbai, &cmd)
            != VK_SUCCESS)
        goto tri_done;
    VkCommandBufferBeginInfo cbbi;
    memset(&cbbi, 0, sizeof(cbbi));
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g_xpu_vk.vkBeginCommandBuffer(cmd, &cbbi);
    g_xpu_vk.vkCmdBuildAccelerationStructuresKHR(cmd, 1, &bgi, &pranges);
    g_xpu_vk.vkEndCommandBuffer(cmd);
    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (g_xpu_vk.vkQueueSubmit(g_xpu_vk.queue, 1, &si, VK_NULL_HANDLE)
            != VK_SUCCESS)
        goto tri_done;
    g_xpu_vk.vkQueueWaitIdle(g_xpu_vk.queue);

    // 7. The BLAS is built (cmd held it). Free that cmd and build the TLAS over one
    //    instance referencing the BLAS, in a SECOND submit — the BLAS wait above
    //    fully synchronizes, so no intra-buffer barrier is needed before the build.
    g_xpu_vk.vkFreeCommandBuffers(g_xpu_vk.device, g_xpu_vk.cmdPool, 1, &cmd);
    cmd = VK_NULL_HANDLE;

    VkAccelerationStructureDeviceAddressInfoKHR blAddrInfo;
    memset(&blAddrInfo, 0, sizeof(blAddrInfo));
    blAddrInfo.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    blAddrInfo.accelerationStructure = accel;   // the BLAS
    VkDeviceAddress blasAddr =
        g_xpu_vk.vkGetAccelerationStructureDeviceAddressKHR(g_xpu_vk.device,
                                                            &blAddrInfo);

    VkAccelerationStructureInstanceKHR inst;
    memset(&inst, 0, sizeof(inst));
    inst.transform.matrix[0][0] = 1.0f;        // identity 3x4 (row-major)
    inst.transform.matrix[1][1] = 1.0f;
    inst.transform.matrix[2][2] = 1.0f;
    inst.mask = 0xFF;                          // matches the kernel's cullMask
    inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    inst.accelerationStructureReference = (uint64_t) blasAddr;
    if (!cajeta_xpu_vk_make_addr_buffer(
            sizeof(inst),
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &instBuf, &instMem, &instMapped))
        goto tri_done;
    memcpy(instMapped, &inst, sizeof(inst));

    VkAccelerationStructureGeometryKHR tlGeom;
    memset(&tlGeom, 0, sizeof(tlGeom));
    tlGeom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    tlGeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlGeom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    tlGeom.geometry.instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tlGeom.geometry.instances.arrayOfPointers = VK_FALSE;
    tlGeom.geometry.instances.data.deviceAddress = cajeta_xpu_vk_buf_addr(instBuf);

    VkAccelerationStructureBuildGeometryInfoKHR tlBgi;
    memset(&tlBgi, 0, sizeof(tlBgi));
    tlBgi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    tlBgi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlBgi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tlBgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlBgi.geometryCount = 1;
    tlBgi.pGeometries = &tlGeom;

    uint32_t tlPrim = 1;
    VkAccelerationStructureBuildSizesInfoKHR tlSizes;
    memset(&tlSizes, 0, sizeof(tlSizes));
    tlSizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    g_xpu_vk.vkGetAccelerationStructureBuildSizesKHR(
        g_xpu_vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlBgi,
        &tlPrim, &tlSizes);
    if (tlSizes.accelerationStructureSize == 0 || tlSizes.buildScratchSize == 0)
        goto tri_done;
    if (!cajeta_xpu_vk_make_addr_buffer(
            tlSizes.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &tlasBuf, &tlasMem, NULL))
        goto tri_done;
    VkAccelerationStructureCreateInfoKHR tlAci;
    memset(&tlAci, 0, sizeof(tlAci));
    tlAci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    tlAci.buffer = tlasBuf;
    tlAci.size = tlSizes.accelerationStructureSize;
    tlAci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    if (g_xpu_vk.vkCreateAccelerationStructureKHR(g_xpu_vk.device, &tlAci, NULL,
                                                  &tlas) != VK_SUCCESS)
        goto tri_done;
    if (!cajeta_xpu_vk_make_addr_buffer(tlSizes.buildScratchSize + scratchAlign - 1,
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                        &tlScratch, &tlScratchMem, NULL))
        goto tri_done;
    tlBgi.dstAccelerationStructure = tlas;
    {
        VkDeviceAddress s = cajeta_xpu_vk_buf_addr(tlScratch);
        s = (s + scratchAlign - 1) & ~((VkDeviceAddress) scratchAlign - 1);
        tlBgi.scratchData.deviceAddress = s;
    }
    VkAccelerationStructureBuildRangeInfoKHR tlRange;
    memset(&tlRange, 0, sizeof(tlRange));
    tlRange.primitiveCount = 1;
    const VkAccelerationStructureBuildRangeInfoKHR* tlRanges = &tlRange;

    VkCommandBufferAllocateInfo tcbai;
    memset(&tcbai, 0, sizeof(tcbai));
    tcbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    tcbai.commandPool = g_xpu_vk.cmdPool;
    tcbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    tcbai.commandBufferCount = 1;
    if (g_xpu_vk.vkAllocateCommandBuffers(g_xpu_vk.device, &tcbai, &cmd)
            != VK_SUCCESS)
        goto tri_done;
    VkCommandBufferBeginInfo tcbbi;
    memset(&tcbbi, 0, sizeof(tcbbi));
    tcbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    tcbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g_xpu_vk.vkBeginCommandBuffer(cmd, &tcbbi);
    g_xpu_vk.vkCmdBuildAccelerationStructuresKHR(cmd, 1, &tlBgi, &tlRanges);
    {   // TLAS write -> ray-query (compute) read availability/visibility.
        VkMemoryBarrier b;
        memset(&b, 0, sizeof(b));
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        b.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        g_xpu_vk.vkCmdPipelineBarrier(
            cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0, NULL, 0, NULL);
    }
    g_xpu_vk.vkEndCommandBuffer(cmd);
    VkSubmitInfo tsi;
    memset(&tsi, 0, sizeof(tsi));
    tsi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    tsi.commandBufferCount = 1;
    tsi.pCommandBuffers = &cmd;
    if (g_xpu_vk.vkQueueSubmit(g_xpu_vk.queue, 1, &tsi, VK_NULL_HANDLE) != VK_SUCCESS)
        goto tri_done;
    g_xpu_vk.vkQueueWaitIdle(g_xpu_vk.queue);

    // 8. Record the AS: the TLAS is bound for ray query; the BLAS it references is
    //    kept alive (asBuf/asMem back the BLAS, tlasBuf/tlasMem the TLAS).
    {
        int slot = -1;
        for (int i = 0; i < g_vk_accel_count; ++i)
            if (!g_vk_accels[i].live) { slot = i; break; }
        if (slot < 0) {
            if (g_vk_accel_count >= CAJETA_VK_MAX_ACCELS) goto tri_done;
            slot = g_vk_accel_count++;
        }
        g_vk_accels[slot].accel = tlas;
        g_vk_accels[slot].asBuf = tlasBuf;
        g_vk_accels[slot].asMem = tlasMem;
        g_vk_accels[slot].blas = accel;
        g_vk_accels[slot].blasBuf = asBuf;
        g_vk_accels[slot].blasMem = asMem;
        g_vk_accels[slot].live = 1;
        result = (int64_t) (slot + 1);
        tlas = VK_NULL_HANDLE; tlasBuf = VK_NULL_HANDLE; tlasMem = VK_NULL_HANDLE;
        accel = VK_NULL_HANDLE; asBuf = VK_NULL_HANDLE; asMem = VK_NULL_HANDLE;
    }

tri_done:
    if (cmd) g_xpu_vk.vkFreeCommandBuffers(g_xpu_vk.device, g_xpu_vk.cmdPool, 1, &cmd);
    if (scratchBuf) g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, scratchBuf, NULL);
    if (scratchMem) g_xpu_vk.vkFreeMemory(g_xpu_vk.device, scratchMem, NULL);
    if (tlScratch) g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, tlScratch, NULL);
    if (tlScratchMem) g_xpu_vk.vkFreeMemory(g_xpu_vk.device, tlScratchMem, NULL);
    if (vMapped) g_xpu_vk.vkUnmapMemory(g_xpu_vk.device, vmem);
    if (vbuf) g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, vbuf, NULL);
    if (vmem) g_xpu_vk.vkFreeMemory(g_xpu_vk.device, vmem, NULL);
    if (instMapped) g_xpu_vk.vkUnmapMemory(g_xpu_vk.device, instMem);
    if (instBuf) g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, instBuf, NULL);
    if (instMem) g_xpu_vk.vkFreeMemory(g_xpu_vk.device, instMem, NULL);
    // On failure these survive (success cleared them); tear them down.
    if (tlas) g_xpu_vk.vkDestroyAccelerationStructureKHR(g_xpu_vk.device, tlas, NULL);
    if (tlasBuf) g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, tlasBuf, NULL);
    if (tlasMem) g_xpu_vk.vkFreeMemory(g_xpu_vk.device, tlasMem, NULL);
    if (accel) g_xpu_vk.vkDestroyAccelerationStructureKHR(g_xpu_vk.device, accel, NULL);
    if (asBuf) g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, asBuf, NULL);
    if (asMem) g_xpu_vk.vkFreeMemory(g_xpu_vk.device, asMem, NULL);
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
    return result;
}

static void cajeta_xpu_vk_accel_free(int64_t handle) {
    pthread_mutex_lock(&g_xpu_vk_submit_mu);   // serialize vs build/launch + table
    struct cajeta_vk_accel* a = cajeta_xpu_vk_accel_rec(handle);
    if (!a) { pthread_mutex_unlock(&g_xpu_vk_submit_mu); return; }
    // Tear down the TLAS then the BLAS it referenced (+ each one's backing store).
    if (a->accel)
        g_xpu_vk.vkDestroyAccelerationStructureKHR(g_xpu_vk.device, a->accel, NULL);
    if (a->asBuf) g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, a->asBuf, NULL);
    if (a->asMem) g_xpu_vk.vkFreeMemory(g_xpu_vk.device, a->asMem, NULL);
    if (a->blas)
        g_xpu_vk.vkDestroyAccelerationStructureKHR(g_xpu_vk.device, a->blas, NULL);
    if (a->blasBuf) g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, a->blasBuf, NULL);
    if (a->blasMem) g_xpu_vk.vkFreeMemory(g_xpu_vk.device, a->blasMem, NULL);
    a->accel = VK_NULL_HANDLE; a->asBuf = VK_NULL_HANDLE; a->asMem = VK_NULL_HANDLE;
    a->blas = VK_NULL_HANDLE; a->blasBuf = VK_NULL_HANDLE; a->blasMem = VK_NULL_HANDLE;
    a->live = 0;
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
}

// One dispatch: shader module + descriptor set (binding i = bindings[i]) +
// pipeline + command buffer + submit + wait. `bindings` are 1-based table
// handles, in kernel-parameter order. Mirrors VulkanDriver::launch.

static VkDescriptorType cajeta_vkb_desc_type(uint8_t kind) {
    if (kind == CAJ_VKB_TEXTURE) return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    if (kind == CAJ_VKB_STORAGE_IMAGE) return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    if (kind == CAJ_VKB_SAMPLER) return VK_DESCRIPTOR_TYPE_SAMPLER;
    if (kind == CAJ_VKB_ACCEL) return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
}

// ---- Pipeline cache --------------------------------------------------------
// v1 created shader module + layouts + pipeline on EVERY launch and destroyed
// them after — RADV recompiled every kernel every dispatch, most of the
// ~200 ms/token the 8B decode measured. Pipelines are keyed on the kernel blob
// plus everything baked at creation (block dims, dynamic-shared size, user
// spec values, binding kinds); the grid is a vkCmdDispatch argument and is NOT
// part of the key. Entries live for the process — the engine's kernel set is
// small (~100) and models churn buffers, not pipelines.
// ---- SPIR-V writes-mask scan -----------------------------------------------
// Which descriptor bindings can a kernel STORE through? Scanned once per
// pipeline from the module's instruction stream: pointer chains are followed
// from OpVariable roots through access chains/copies, and every store-shaped
// opcode (OpStore, OpCopyMemory*, the writing OpAtomic*s) marks its root's
// binding. Pointers escaping into OpFunctionCall/OpExtInst/OpPhi/OpSelect are
// marked without being followed. A defeated scan returns all-ones — a wrong
// mask can only over-synchronize, never under-synchronize. This feeds the
// batched path's barrier elision (llama.cpp's ggml-vulkan does the same
// dependency test from ggml tensor metadata; we recover it from the SPIR-V).
static uint64_t caj_vk_spv_written_mask(const uint32_t* w, size_t nwords) {
    if (nwords < 6 || w[0] != 0x07230203u) return ~0ull;
    uint32_t bound = w[3];
    if (bound == 0 || bound > (1u << 22)) return ~0ull;
    uint32_t* root = (uint32_t*) calloc(bound, sizeof(uint32_t));
    uint32_t* bind = (uint32_t*) calloc(bound, sizeof(uint32_t)); // binding+1
    uint8_t* isBuf = (uint8_t*) calloc(bound, 1);
    if (!root || !bind || !isBuf) {
        free(root); free(bind); free(isBuf);
        return ~0ull;
    }
    uint64_t mask = 0;
    int bad = 0;
#define CAJ_SPV_MARK(id)                                                       \
    do {                                                                       \
        uint32_t r_ = ((id) < bound) ? root[(id)] : 0;                         \
        if (r_ && isBuf[r_] && bind[r_]) {                                     \
            uint32_t b_ = bind[r_] - 1;                                        \
            if (b_ < 64) mask |= 1ull << b_; else bad = 1;                     \
        }                                                                      \
    } while (0)
    size_t i = 5;
    while (i < nwords) {
        uint32_t op = w[i] & 0xFFFFu;
        uint32_t wc = w[i] >> 16;
        if (wc == 0 || i + wc > nwords) { bad = 1; break; }
        const uint32_t* a = &w[i];
        switch (op) {
        case 71: // OpDecorate
            if (wc >= 4 && a[2] == 33 /*Binding*/ && a[1] < bound)
                bind[a[1]] = a[3] + 1;
            break;
        case 59: // OpVariable: result a[2], storage class a[3]
            if (wc >= 4 && a[2] < bound) {
                root[a[2]] = a[2];
                if (a[3] == 12 /*StorageBuffer*/ || a[3] == 2 /*Uniform*/)
                    isBuf[a[2]] = 1;
            }
            break;
        case 65: case 66: case 67: case 70: // access chains: base a[3]
            if (wc >= 4 && a[2] < bound && a[3] < bound)
                root[a[2]] = root[a[3]];
            break;
        case 83: case 124: // OpCopyObject / OpBitcast: operand a[3]
            if (wc >= 4 && a[2] < bound && a[3] < bound)
                root[a[2]] = root[a[3]];
            break;
        case 62: case 63: // OpStore / OpCopyMemory: target a[1]
            if (wc >= 3) CAJ_SPV_MARK(a[1]);
            break;
        case 64: // OpCopyMemorySized: target a[1]
            if (wc >= 4) CAJ_SPV_MARK(a[1]);
            break;
        case 228: // OpAtomicStore: pointer a[1]
            if (wc >= 5) CAJ_SPV_MARK(a[1]);
            break;
        case 319: // OpAtomicFlagClear: pointer a[1]
            if (wc >= 4) CAJ_SPV_MARK(a[1]);
            break;
        case 318: // OpAtomicFlagTestAndSet: pointer a[3]
            if (wc >= 6) CAJ_SPV_MARK(a[3]);
            break;
        case 4458: // OpCooperativeMatrixStoreKHR: pointer a[1]
        case 5360: // OpCooperativeMatrixStoreNV: pointer a[1]
            if (wc >= 3) CAJ_SPV_MARK(a[1]);
            break;
        case 229: case 230: case 231: case 232: case 233: case 234:
        case 235: case 236: case 237: case 238: case 239: case 240:
        case 241: case 242: // result-carrying atomics: pointer a[3]
        case 5614: case 5615: case 6035: // FMin/FMax/FAdd EXT
            if (wc >= 4) CAJ_SPV_MARK(a[3]);
            break;
        case 57: // OpFunctionCall: a pointer arg escapes the scan — mark it
            for (uint32_t k = 4; k < wc; ++k)
                if (a[k] < bound && root[a[k]]) CAJ_SPV_MARK(a[k]);
            break;
        case 12: // OpExtInst (Modf/Frexp write through a pointer operand)
            for (uint32_t k = 5; k < wc; ++k)
                if (a[k] < bound && root[a[k]]) CAJ_SPV_MARK(a[k]);
            break;
        case 169: // OpSelect over pointers: conservative
            if (wc >= 6) {
                if (a[4] < bound && root[a[4]]) CAJ_SPV_MARK(a[4]);
                if (a[5] < bound && root[a[5]]) CAJ_SPV_MARK(a[5]);
            }
            break;
        case 245: // OpPhi over pointers: conservative
            for (uint32_t k = 3; k < wc; k += 2)
                if (a[k] < bound && root[a[k]]) CAJ_SPV_MARK(a[k]);
            break;
        default:
            break;
        }
        i += wc;
    }
#undef CAJ_SPV_MARK
    free(root); free(bind); free(isBuf);
    return bad ? ~0ull : mask;
}

struct caj_vk_pipe {
    int live;
    const void* spirv;
    uint64_t len;
    unsigned bx, by, bz, sharedBytes;
    int nspec;
    int32_t spec[60];
    int n;
    uint8_t kinds[64];
    uint64_t writesMask;   // bindings the kernel can store through (bit i)
    VkShaderModule module;
    VkDescriptorSetLayout setLayout;
    VkPipelineLayout pipeLayout;
    VkPipeline pipeline;
};
#define CAJ_VK_PIPES 1024
static struct caj_vk_pipe g_vk_pipes[CAJ_VK_PIPES];
static int g_vk_pipe_count;

static struct caj_vk_pipe* caj_vk_pipe_get(
        const void* spirv, uint64_t len, const char* entry,
        const uint8_t* kinds, int n,
        unsigned bx, unsigned by, unsigned bz, unsigned sharedBytes,
        int userSpecCount, const int32_t* userSpecValues) {
    int nUser = userSpecCount;
    if (nUser < 0) nUser = 0;
    if (nUser > 60) nUser = 60;
    for (int i = 0; i < g_vk_pipe_count; ++i) {
        struct caj_vk_pipe* P = &g_vk_pipes[i];
        if (!P->live || P->spirv != spirv || P->len != len) continue;
        if (P->bx != bx || P->by != by || P->bz != bz
                || P->sharedBytes != sharedBytes || P->n != n
                || P->nspec != nUser)
            continue;
        if (nUser && memcmp(P->spec, userSpecValues,
                            (size_t) nUser * sizeof(int32_t)) != 0)
            continue;
        if (memcmp(P->kinds, kinds, (size_t) n) != 0) continue;
        return P;
    }
    if (g_vk_pipe_count >= CAJ_VK_PIPES) {
        fprintf(stderr, "cajeta.xpu.vulkan: pipeline cache FULL (%d) for "
                "kernel '%s'\n", CAJ_VK_PIPES, entry ? entry : "?");
        return NULL;
    }
    struct caj_vk_pipe* P = &g_vk_pipes[g_vk_pipe_count];
    memset(P, 0, sizeof(*P));
    P->spirv = spirv; P->len = len;
    P->bx = bx; P->by = by; P->bz = bz; P->sharedBytes = sharedBytes;
    P->nspec = nUser;
    if (nUser) memcpy(P->spec, userSpecValues,
                      (size_t) nUser * sizeof(int32_t));
    P->n = n;
    memcpy(P->kinds, kinds, (size_t) n);
    P->writesMask = caj_vk_spv_written_mask((const uint32_t*) spirv,
                                            (size_t) (len / 4));

    VkShaderModuleCreateInfo smci;
    memset(&smci, 0, sizeof(smci));
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = (size_t) len;
    smci.pCode = (const uint32_t*) spirv;
    if (g_xpu_vk.vkCreateShaderModule(g_xpu_vk.device, &smci, NULL,
                                      &P->module) != VK_SUCCESS)
        goto fail;

    {
        VkDescriptorSetLayoutBinding binds[64];
        memset(binds, 0, sizeof(binds[0]) * n);
        for (int i = 0; i < n; ++i) {
            binds[i].binding = (uint32_t) i;
            binds[i].descriptorType = cajeta_vkb_desc_type(kinds[i]);
            binds[i].descriptorCount =
                (kinds[i] == CAJ_VKB_BUFFER_ARRAY) ? CAJ_VK_BINDLESS_MAX : 1;
            binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo slci;
        memset(&slci, 0, sizeof(slci));
        slci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        slci.bindingCount = (uint32_t) n;
        slci.pBindings = binds;
        if (g_xpu_vk.vkCreateDescriptorSetLayout(g_xpu_vk.device, &slci, NULL,
                                                 &P->setLayout) != VK_SUCCESS)
            goto fail;
    }
    {
        VkPipelineLayoutCreateInfo plci;
        memset(&plci, 0, sizeof(plci));
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &P->setLayout;
        if (g_xpu_vk.vkCreatePipelineLayout(g_xpu_vk.device, &plci, NULL,
                                            &P->pipeLayout) != VK_SUCCESS)
            goto fail;
    }
    {
        // Spec constants: SpecId 0/1/2 = block dims, SpecId 3 = the dynamic
        // shared array's length in 4-byte elements, SpecId 4+i = the user's
        // Spec.geti/getf slot i (raw 4-byte word). All baked at pipeline
        // creation — which is exactly why they are part of the cache key.
        enum { CAJ_VK_FIRST_USER_SPEC_ID = 4 };
        uint32_t specData[64];
        VkSpecializationMapEntry specEntries[64];
        specData[0] = bx; specData[1] = by; specData[2] = bz;
        specData[3] = sharedBytes / 4u;
        for (int i = 0; i < 4; ++i)
            specEntries[i] = (VkSpecializationMapEntry){
                (uint32_t) i, (uint32_t) (i * sizeof(uint32_t)),
                sizeof(uint32_t) };
        for (int i = 0; i < nUser; ++i) {
            specData[4 + i] = (uint32_t) userSpecValues[i];
            specEntries[4 + i] = (VkSpecializationMapEntry){
                (uint32_t) (CAJ_VK_FIRST_USER_SPEC_ID + i),
                (uint32_t) ((4 + i) * sizeof(uint32_t)), sizeof(uint32_t) };
        }
        VkSpecializationInfo specInfo;
        specInfo.mapEntryCount = (uint32_t) (4 + nUser);
        specInfo.pMapEntries = specEntries;
        specInfo.dataSize = (size_t) (4 + nUser) * sizeof(uint32_t);
        specInfo.pData = specData;

        VkComputePipelineCreateInfo cpci;
        memset(&cpci, 0, sizeof(cpci));
        cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = P->module;
        cpci.stage.pName = entry;
        cpci.stage.pSpecializationInfo = &specInfo;
        cpci.layout = P->pipeLayout;
#if defined(VK_VERSION_1_3)
        // Pin wave32: the kernels' cooperation (barriers, LDS staging, the
        // Wave.* ops) is authored for 32-lane subgroups; RADV's gfx11
        // default of wave64 turns that into barrier divergence — a device
        // HANG on the staged batch GEMMs, then context-lost for the rest
        // of the process. REQUIRE_FULL_SUBGROUPS when the workgroup tiles
        // by 32 keeps the mapping exact.
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rss;
        if (g_xpu_vk.subgroupCtl) {
            /* Wave-width contract: kernels are authored for 32-lane
             * subgroups and pinned there — EXCEPT a kernel whose entry
             * name contains "W64", which declares it is authored for
             * 64-lane subgroups (llama.cpp's mul_mm runs wave64 on RADV;
             * the coop GEMM's W64 twin adopts its 2-warp layout). The
             * name is the contract: it travels inside the SPIR-V blob,
             * so the pipeline cache and every backend see one truth. */
            uint32_t want = 32u;
            if (entry && strstr(entry, "W64")
                    && g_xpu_vk.maxSubgroupSize >= 64u)
                want = 64u;
            memset(&rss, 0, sizeof(rss));
            rss.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO;
            rss.requiredSubgroupSize = want;
            cpci.stage.pNext = &rss;
            if (((uint64_t) bx * by * bz) % want == 0u)
                cpci.stage.flags |=
                    VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
        }
#endif
        if (g_xpu_vk.vkCreateComputePipelines(g_xpu_vk.device, VK_NULL_HANDLE,
                                              1, &cpci, NULL, &P->pipeline)
                != VK_SUCCESS)
            goto fail;
    }
    P->live = 1;
    g_vk_pipe_count++;
    return P;
fail:
    fprintf(stderr, "cajeta.xpu.vulkan: pipeline build FAILED for kernel "
            "'%s'\n", entry ? entry : "?");
    if (P->pipeline) g_xpu_vk.vkDestroyPipeline(g_xpu_vk.device, P->pipeline, NULL);
    if (P->pipeLayout) g_xpu_vk.vkDestroyPipelineLayout(g_xpu_vk.device, P->pipeLayout, NULL);
    if (P->setLayout) g_xpu_vk.vkDestroyDescriptorSetLayout(g_xpu_vk.device, P->setLayout, NULL);
    if (P->module) g_xpu_vk.vkDestroyShaderModule(g_xpu_vk.device, P->module, NULL);
    memset(P, 0, sizeof(*P));
    return NULL;
}

// ---- Descriptor marshalling (shared by the eager and batched paths) --------
// Fills and applies the descriptor writes for one dispatch. Static scratch —
// callers hold g_xpu_vk_submit_mu. When `noteBound` is set, every referenced
// VkBuffer lands in the open batch's bound-set (the host-access hazard test).
static struct {
    VkDescriptorBufferInfo bufInfos[64];
    VkDescriptorBufferInfo arrInfos[64 * CAJ_VK_BINDLESS_MAX];
    VkDescriptorImageInfo imgInfos[64];
    VkWriteDescriptorSet writes[64];
    VkWriteDescriptorSetAccelerationStructureKHR accelInfos[64];
    VkAccelerationStructureKHR accelHandles[64];
    // Per-binding byte ranges of THIS dispatch, harvested for barrier
    // elision. A view binds [view_offset, parent end) (descriptors use
    // VK_WHOLE_SIZE), so that open-ended span is the honest bound.
    VkBuffer rngBuf[64];
    VkDeviceSize rngOff[64];
    VkDeviceSize rngEnd[64];
    uint8_t rngIsBuf[64];
    int sawImage;      // dispatch binds an image (sampled or storage)
    int sawArray;      // dispatch binds a bindless buffer array
} g_vk_ws;

static int caj_vk_marshal_writes(VkDescriptorSet descSet,
                                 const int64_t* bindings,
                                 const uint8_t* kinds, int n, int noteBound) {
    memset(g_vk_ws.writes, 0, sizeof(g_vk_ws.writes[0]) * n);
    memset(g_vk_ws.imgInfos, 0, sizeof(g_vk_ws.imgInfos[0]) * n);
    memset(g_vk_ws.accelInfos, 0, sizeof(g_vk_ws.accelInfos[0]) * n);
    memset(g_vk_ws.rngIsBuf, 0, sizeof(g_vk_ws.rngIsBuf[0]) * n);
    g_vk_ws.sawImage = 0;
    g_vk_ws.sawArray = 0;
    for (int i = 0; i < n; ++i) {
        VkWriteDescriptorSet* w = &g_vk_ws.writes[i];
        w->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w->dstSet = descSet;
        w->dstBinding = (uint32_t) i;
        w->descriptorCount = 1;
        w->descriptorType = cajeta_vkb_desc_type(kinds[i]);
        if (kinds[i] == CAJ_VKB_ACCEL) {
            struct cajeta_vk_accel* a = cajeta_xpu_vk_accel_rec(bindings[i]);
            if (!a) return 0;
            g_vk_ws.accelHandles[i] = a->accel;
            g_vk_ws.accelInfos[i].sType =
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
            g_vk_ws.accelInfos[i].accelerationStructureCount = 1;
            g_vk_ws.accelInfos[i].pAccelerationStructures =
                &g_vk_ws.accelHandles[i];
            w->pNext = &g_vk_ws.accelInfos[i];
        } else if (kinds[i] == CAJ_VKB_TEXTURE) {
            struct cajeta_vk_tex* t = cajeta_xpu_vk_tex_rec(bindings[i]);
            if (!t) return 0;
            g_vk_ws.sawImage = 1;
            g_vk_ws.imgInfos[i].imageView = t->view;
            g_vk_ws.imgInfos[i].imageLayout =
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            w->pImageInfo = &g_vk_ws.imgInfos[i];
        } else if (kinds[i] == CAJ_VKB_STORAGE_IMAGE) {
            // Bound in GENERAL — the only layout valid for OpImageWrite; the
            // recorded pre-dispatch barrier transitions it.
            struct cajeta_vk_tex* t = cajeta_xpu_vk_tex_rec(bindings[i]);
            if (!t) return 0;
            g_vk_ws.sawImage = 1;
            g_vk_ws.imgInfos[i].imageView = t->view;
            g_vk_ws.imgInfos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            w->pImageInfo = &g_vk_ws.imgInfos[i];
        } else if (kinds[i] == CAJ_VKB_SAMPLER) {
            g_vk_ws.imgInfos[i].sampler = (VkSampler) (uintptr_t) bindings[i];
            if (g_vk_ws.imgInfos[i].sampler == VK_NULL_HANDLE) return 0;
            w->pImageInfo = &g_vk_ws.imgInfos[i];
        } else if (kinds[i] == CAJ_VKB_BUFFER_ARRAY) {
            // bindings[i] -> launch-marshalled [count, h0 .. h(count-1)]; bind
            // CAJ_VK_BINDLESS_MAX descriptors, padding with element 0 so every
            // slot is bound (no PARTIALLY_BOUND requirement).
            const int64_t* arr = (const int64_t*) (intptr_t) bindings[i];
            int64_t cnt = arr ? arr[0] : 0;
            if (cnt < 1 || cnt > CAJ_VK_BINDLESS_MAX) return 0;
            g_vk_ws.sawArray = 1;
            VkDescriptorBufferInfo* row =
                &g_vk_ws.arrInfos[i * CAJ_VK_BINDLESS_MAX];
            for (int e = 0; e < CAJ_VK_BINDLESS_MAX; ++e) {
                int64_t h = arr[1 + (e < (int) cnt ? e : 0)];
                struct cajeta_vk_buf* r = cajeta_xpu_vk_rec(h);
                if (!r) return 0;
                row[e].buffer = r->buffer;
                row[e].offset = r->view_offset;
                row[e].range = VK_WHOLE_SIZE;
                if (noteBound) caj_vk_bound_note((void*) r->buffer);
            }
            w->descriptorCount = CAJ_VK_BINDLESS_MAX;
            w->pBufferInfo = row;
        } else {
            struct cajeta_vk_buf* r = cajeta_xpu_vk_rec(bindings[i]);
            if (!r) return 0;
            g_vk_ws.bufInfos[i].buffer = r->buffer;
            g_vk_ws.bufInfos[i].offset = r->view_offset;
            g_vk_ws.bufInfos[i].range = VK_WHOLE_SIZE;
            g_vk_ws.rngBuf[i] = r->buffer;
            g_vk_ws.rngOff[i] = r->view_offset;
            g_vk_ws.rngEnd[i] = r->view_offset + r->size;
            g_vk_ws.rngIsBuf[i] = 1;
            w->pBufferInfo = &g_vk_ws.bufInfos[i];
            if (noteBound) caj_vk_bound_note((void*) r->buffer);
        }
    }
    g_xpu_vk.vkUpdateDescriptorSets(g_xpu_vk.device, (uint32_t) n,
                                    g_vk_ws.writes, 0, NULL);
    return 1;
}

// Storage images must be in GENERAL before dispatch; when already GENERAL the
// barrier is a write->read/write dependency instead of a transition, so a
// kernel reading a previous dispatch's img.store sees the new texels.
static void caj_vk_record_image_transitions(VkCommandBuffer cmd,
                                            const int64_t* bindings,
                                            const uint8_t* kinds, int n) {
    for (int i = 0; i < n; ++i) {
        if (kinds[i] != CAJ_VKB_STORAGE_IMAGE) continue;
        struct cajeta_vk_tex* t = cajeta_xpu_vk_tex_rec(bindings[i]);
        if (!t) continue;
        int wasGeneral = (t->layout == VK_IMAGE_LAYOUT_GENERAL);
        VkImageMemoryBarrier toGen;
        memset(&toGen, 0, sizeof(toGen));
        toGen.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toGen.oldLayout = t->layout;
        toGen.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGen.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGen.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGen.image = t->image;
        toGen.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toGen.subresourceRange.levelCount = 1;
        toGen.subresourceRange.layerCount = 1;
        toGen.srcAccessMask = wasGeneral ? VK_ACCESS_SHADER_WRITE_BIT : 0;
        toGen.dstAccessMask =
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        g_xpu_vk.vkCmdPipelineBarrier(
            cmd,
            wasGeneral ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                       : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
            0, NULL, 0, NULL, 1, &toGen);
        t->layout = VK_IMAGE_LAYOUT_GENERAL;
    }
}

// ---- Batch machinery -------------------------------------------------------
static VkDescriptorPool caj_vk_batch_make_pool(void) {
    VkDescriptorPoolSize sizes[5];
    sizes[0] = (VkDescriptorPoolSize){ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096 };
    sizes[1] = (VkDescriptorPoolSize){ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 128 };
    sizes[2] = (VkDescriptorPoolSize){ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 128 };
    sizes[3] = (VkDescriptorPoolSize){ VK_DESCRIPTOR_TYPE_SAMPLER, 64 };
    sizes[4] = (VkDescriptorPoolSize){
        VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 32 };
    VkDescriptorPoolCreateInfo dpci;
    memset(&dpci, 0, sizeof(dpci));
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 256;
    dpci.poolSizeCount = g_xpu_vk.rayQuery ? 5 : 4;
    dpci.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (g_xpu_vk.vkCreateDescriptorPool(g_xpu_vk.device, &dpci, NULL, &pool)
            != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return pool;
}

// Allocate one set from the batch's pool chain, growing it on exhaustion.
static int caj_vk_batch_alloc_set(VkDescriptorSetLayout layout,
                                  VkDescriptorSet* out) {
    for (;;) {
        if (g_vk_batch.poolCursor >= g_vk_batch.npools) {
            if (g_vk_batch.npools >= CAJ_VK_BATCH_POOLS) return 0;
            VkDescriptorPool pool = caj_vk_batch_make_pool();
            if (!pool) return 0;
            g_vk_batch.pools[g_vk_batch.npools++].pool = pool;
        }
        VkDescriptorSetAllocateInfo dsai;
        memset(&dsai, 0, sizeof(dsai));
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = g_vk_batch.pools[g_vk_batch.poolCursor].pool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &layout;
        VkResult r = g_xpu_vk.vkAllocateDescriptorSets(g_xpu_vk.device, &dsai,
                                                       out);
        if (r == VK_SUCCESS) return 1;
        g_vk_batch.poolCursor++;   // pool exhausted (or fragmented): next
    }
}

// ---- Barrier elision -------------------------------------------------------
// llama.cpp's scheme (ggml-vulkan.cpp ggml_vk_build_graph): keep the byte
// ranges written and read since the last barrier; a new dispatch needs one
// only when a binding overlaps an unsynced WRITE (RAW/WAW), or one of its own
// writes overlaps an unsynced READ (WAR). Read-after-read never barriers, so
// the q/k/v and gate/up projections overlap on the GPU. Which bindings a
// kernel writes comes from the pipe's SPIR-V writes-mask scan. Anything the
// tracker cannot see — images, bindless arrays, a defeated scan, list
// overflow — falls back to a barrier. CAJETA_XPU_VK_NOELIDE=1 restores the
// barrier-every-dispatch behaviour (the control arm); CAJETA_XPU_VK_SYNC_LOG=1
// prints dispatch/barrier counts at each flush.
struct caj_vk_range { VkBuffer buf; VkDeviceSize off, end; };
#define CAJ_VK_UNSYNC_W 192
#define CAJ_VK_UNSYNC_R 384
static struct {
    struct caj_vk_range w[CAJ_VK_UNSYNC_W]; int nw;
    struct caj_vk_range r[CAJ_VK_UNSYNC_R]; int nr;
} g_vk_unsync;
static int g_vk_elide = -1;      // -1 unread, 0 off (control), 1 on
static int g_vk_sync_log = 0;
static unsigned g_vk_stat_disp, g_vk_stat_barrier;

static void caj_vk_unsync_clear(void) {
    g_vk_unsync.nw = 0;
    g_vk_unsync.nr = 0;
}
static int caj_vk_range_hits(const struct caj_vk_range* list, int n,
                             VkBuffer buf, VkDeviceSize off,
                             VkDeviceSize end) {
    for (int i = 0; i < n; ++i)
        if (list[i].buf == buf && list[i].off < end && off < list[i].end)
            return 1;
    return 0;
}

static int caj_vk_batch_begin(void) {
    if (g_vk_batch.open) return 1;
    if (!g_vk_batch.inited) {
        VkCommandBufferAllocateInfo cbai;
        memset(&cbai, 0, sizeof(cbai));
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = g_xpu_vk.cmdPool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = CAJ_VK_BATCH_CMDS;
        if (g_xpu_vk.vkAllocateCommandBuffers(g_xpu_vk.device, &cbai,
                                              g_vk_batch.cmds) != VK_SUCCESS)
            return 0;
        VkFenceCreateInfo fci;
        memset(&fci, 0, sizeof(fci));
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (g_xpu_vk.vkCreateFence(g_xpu_vk.device, &fci, NULL,
                                   &g_vk_batch.fence) != VK_SUCCESS)
            return 0;
        g_vk_batch.inited = 1;
    }
    VkCommandBufferBeginInfo cbbi;
    memset(&cbbi, 0, sizeof(cbbi));
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g_vk_batch.cur = 0;
    if (g_xpu_vk.vkBeginCommandBuffer(g_vk_batch.cmds[0], &cbbi) != VK_SUCCESS)
        return 0;
    g_vk_batch.open = 1;
    g_vk_batch.dispatches = 0;
    g_vk_batch.chunkDispatches = 0;
    g_vk_batch.chunksFlown = 0;
    g_vk_batch.poolCursor = 0;
    memset(g_vk_batch.bound, 0, sizeof(g_vk_batch.bound));
    g_vk_batch.nbound = 0;
    g_vk_batch.boundOverflow = 0;
    caj_vk_unsync_clear();   // flush waited on the fence: GPU is idle
    return 1;
}

// Hand the open chunk to the GPU (fenceless) and continue recording into the
// next ring slot. On ring exhaustion, wait everything out via an empty
// fence-carrying submit — queue order makes the fence cover all prior
// chunks — then recycle the ring (the pool's RESET_COMMAND_BUFFER_BIT lets
// vkBeginCommandBuffer implicitly reset a completed buffer).
static void caj_vk_batch_submit_chunk(void) {
    if (!g_vk_batch.open || g_vk_batch.chunkDispatches == 0) return;
    g_xpu_vk.vkEndCommandBuffer(g_vk_batch.cmds[g_vk_batch.cur]);
    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_vk_batch.cmds[g_vk_batch.cur];
    if (g_xpu_vk.vkQueueSubmit(g_xpu_vk.queue, 1, &si, VK_NULL_HANDLE)
            != VK_SUCCESS)
        fprintf(stderr, "cajeta.xpu.vulkan: chunk submit FAILED "
                "(%u dispatches dropped)\n", g_vk_batch.chunkDispatches);
    g_vk_batch.chunksFlown++;
    g_vk_batch.chunkDispatches = 0;
    g_vk_batch.cur++;
    if (g_vk_batch.cur >= CAJ_VK_BATCH_CMDS) {
        VkSubmitInfo empty;
        memset(&empty, 0, sizeof(empty));
        empty.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        if (g_xpu_vk.vkQueueSubmit(g_xpu_vk.queue, 1, &empty,
                                   g_vk_batch.fence) == VK_SUCCESS) {
            g_xpu_vk.vkWaitForFences(g_xpu_vk.device, 1, &g_vk_batch.fence,
                                     VK_TRUE, ~0ull);
            g_xpu_vk.vkResetFences(g_xpu_vk.device, 1, &g_vk_batch.fence);
        }
        // Everything recorded so far has executed: the descriptor pools can
        // recycle too (nothing recorded-but-unsubmitted references a set).
        for (int i = 0; i < g_vk_batch.npools; ++i)
            g_xpu_vk.vkResetDescriptorPool(g_xpu_vk.device,
                                           g_vk_batch.pools[i].pool, 0);
        g_vk_batch.poolCursor = 0;
        g_vk_batch.cur = 0;
    }
    VkCommandBufferBeginInfo cbbi;
    memset(&cbbi, 0, sizeof(cbbi));
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (g_xpu_vk.vkBeginCommandBuffer(g_vk_batch.cmds[g_vk_batch.cur], &cbbi)
            != VK_SUCCESS) {
        fprintf(stderr, "cajeta.xpu.vulkan: chunk begin FAILED — "
                "batch closed early\n");
        g_vk_batch.open = 0;
    }
}

// Land the open batch: final compute->host barrier, one submit, fence wait,
// then reclaim (descriptor pools reset for reuse, deferred frees executed).
static void cajeta_xpu_vk_flush(void) {
    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    if (!g_vk_batch.open) {
        pthread_mutex_unlock(&g_xpu_vk_submit_mu);
        return;
    }
    g_vk_batch.open = 0;   // no re-entry through the frees below
    if (g_vk_batch.dispatches > 0) {
        VkMemoryBarrier mb;
        memset(&mb, 0, sizeof(mb));
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        g_xpu_vk.vkCmdPipelineBarrier(
            g_vk_batch.cmds[g_vk_batch.cur],
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
    g_xpu_vk.vkEndCommandBuffer(g_vk_batch.cmds[g_vk_batch.cur]);
    // The final submit carries the fence; queue order makes it cover every
    // fenceless chunk that flew before it. Submit even when the last chunk
    // is empty if any chunk is in flight — the host is about to read.
    if (g_vk_batch.chunkDispatches > 0 || g_vk_batch.chunksFlown > 0) {
        VkSubmitInfo si;
        memset(&si, 0, sizeof(si));
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &g_vk_batch.cmds[g_vk_batch.cur];
        if (g_xpu_vk.vkQueueSubmit(g_xpu_vk.queue, 1, &si, g_vk_batch.fence)
                == VK_SUCCESS) {
            VkResult wr = g_xpu_vk.vkWaitForFences(
                g_xpu_vk.device, 1, &g_vk_batch.fence, VK_TRUE, ~0ull);
            if (wr != VK_SUCCESS)
                fprintf(stderr, "cajeta.xpu.vulkan: batch fence wait FAILED "
                        "(%d) after %u dispatches\n", (int) wr,
                        g_vk_batch.dispatches);
            g_xpu_vk.vkResetFences(g_xpu_vk.device, 1, &g_vk_batch.fence);
        } else {
            fprintf(stderr, "cajeta.xpu.vulkan: batch submit FAILED "
                    "(%u dispatches dropped)\n", g_vk_batch.chunkDispatches);
        }
    }
    if (g_vk_sync_log && g_vk_stat_disp) {
        fprintf(stderr, "cajeta.xpu.vulkan: sync-log %u dispatches, "
                "%u barriers elided to %u\n", g_vk_stat_disp,
                g_vk_stat_disp > 0 ? g_vk_stat_disp - 1 : 0,
                g_vk_stat_barrier);
        g_vk_stat_disp = 0;
        g_vk_stat_barrier = 0;
    }
    for (int i = 0; i < g_vk_batch.npools; ++i)
        g_xpu_vk.vkResetDescriptorPool(g_xpu_vk.device,
                                       g_vk_batch.pools[i].pool, 0);
    g_vk_batch.poolCursor = 0;
    for (int i = 0; i < g_vk_batch.npendingFree; ++i)
        cajeta_xpu_vk_free_now(g_vk_batch.pendingFree[i]);
    g_vk_batch.npendingFree = 0;
    for (int i = 0; i < g_vk_batch.npendingSampler; ++i)
        g_xpu_vk.vkDestroySampler(
            g_xpu_vk.device,
            (VkSampler) (uintptr_t) g_vk_batch.pendingSampler[i], NULL);
    g_vk_batch.npendingSampler = 0;
    g_vk_batch.dispatches = 0;
    g_vk_scalar_arena.cur = 0;   // fence waited: every slot is consumed
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
}

// ---- Batched record path ---------------------------------------------------
static int caj_vk_launch_batched(struct caj_vk_pipe* P,
                                 const int64_t* bindings,
                                 const uint8_t* kinds, int n,
                                 unsigned gx, unsigned gy, unsigned gz) {
    if (!caj_vk_batch_begin()) return 0;
    VkDescriptorSet ds = VK_NULL_HANDLE;
    if (!caj_vk_batch_alloc_set(P->setLayout, &ds)) {
        // Pool chain exhausted mid-batch: land what is recorded, start fresh.
        cajeta_xpu_vk_flush();
        if (!caj_vk_batch_begin()) return 0;
        if (!caj_vk_batch_alloc_set(P->setLayout, &ds)) return 0;
    }
    if (!caj_vk_marshal_writes(ds, bindings, kinds, n, /*noteBound=*/1))
        return 0;
    if (g_vk_elide < 0) {
        g_vk_elide = getenv("CAJETA_XPU_VK_NOELIDE") ? 0 : 1;
        g_vk_sync_log = getenv("CAJETA_XPU_VK_SYNC_LOG") ? 1 : 0;
    }
    int needSync;
    if (!g_vk_elide || g_vk_ws.sawImage || g_vk_ws.sawArray) {
        needSync = 1;   // opaque to the tracker: keep the old full barrier
    } else {
        needSync = 0;
        for (int i = 0; i < n && !needSync; ++i) {
            if (!g_vk_ws.rngIsBuf[i]) continue;
            int isWrite = (int) ((P->writesMask >> i) & 1u);
            // RAW / WAW: anything touching an unsynced write.
            if (caj_vk_range_hits(g_vk_unsync.w, g_vk_unsync.nw,
                                  g_vk_ws.rngBuf[i], g_vk_ws.rngOff[i],
                                  g_vk_ws.rngEnd[i]))
                needSync = 1;
            // WAR: our write over an unsynced read.
            else if (isWrite &&
                     caj_vk_range_hits(g_vk_unsync.r, g_vk_unsync.nr,
                                       g_vk_ws.rngBuf[i], g_vk_ws.rngOff[i],
                                       g_vk_ws.rngEnd[i]))
                needSync = 1;
        }
    }
    if (needSync && g_vk_batch.dispatches > 0) {
        VkMemoryBarrier mb;
        memset(&mb, 0, sizeof(mb));
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        g_xpu_vk.vkCmdPipelineBarrier(
            g_vk_batch.cmds[g_vk_batch.cur], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        g_vk_stat_barrier++;
    }
    if (needSync) caj_vk_unsync_clear();
    // Record this dispatch's ranges for the next decision. If either list
    // cannot hold them, emit one barrier here (it is recorded before this
    // dispatch, so this dispatch's ranges alone seed the fresh lists).
    {
        int nw = 0, nr = 0;
        for (int i = 0; i < n; ++i) {
            if (!g_vk_ws.rngIsBuf[i]) continue;
            if ((P->writesMask >> i) & 1u) nw++; else nr++;
        }
        if (g_vk_unsync.nw + nw > CAJ_VK_UNSYNC_W ||
            g_vk_unsync.nr + nr > CAJ_VK_UNSYNC_R) {
            if (g_vk_batch.dispatches > 0 && !needSync) {
                VkMemoryBarrier mb;
                memset(&mb, 0, sizeof(mb));
                mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                mb.dstAccessMask =
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                g_xpu_vk.vkCmdPipelineBarrier(
                    g_vk_batch.cmds[g_vk_batch.cur], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb,
                    0, NULL, 0, NULL);
                g_vk_stat_barrier++;
            }
            caj_vk_unsync_clear();
        }
        for (int i = 0; i < n; ++i) {
            if (!g_vk_ws.rngIsBuf[i]) continue;
            struct caj_vk_range e = { g_vk_ws.rngBuf[i], g_vk_ws.rngOff[i],
                                      g_vk_ws.rngEnd[i] };
            if ((P->writesMask >> i) & 1u) {
                if (g_vk_unsync.nw < CAJ_VK_UNSYNC_W)
                    g_vk_unsync.w[g_vk_unsync.nw++] = e;
            } else {
                if (g_vk_unsync.nr < CAJ_VK_UNSYNC_R)
                    g_vk_unsync.r[g_vk_unsync.nr++] = e;
            }
        }
    }
    g_vk_stat_disp++;
    caj_vk_record_image_transitions(g_vk_batch.cmds[g_vk_batch.cur], bindings, kinds, n);
    g_xpu_vk.vkCmdBindPipeline(g_vk_batch.cmds[g_vk_batch.cur], VK_PIPELINE_BIND_POINT_COMPUTE,
                               P->pipeline);
    g_xpu_vk.vkCmdBindDescriptorSets(g_vk_batch.cmds[g_vk_batch.cur],
                                     VK_PIPELINE_BIND_POINT_COMPUTE,
                                     P->pipeLayout, 0, 1, &ds, 0, NULL);
    g_xpu_vk.vkCmdDispatch(g_vk_batch.cmds[g_vk_batch.cur], gx, gy, gz);
    g_vk_batch.dispatches++;
    g_vk_batch.chunkDispatches++;
    {
        static int s_chunk = -1;
        if (s_chunk < 0)
            s_chunk = getenv("CAJETA_XPU_VK_NOCHUNK")
                          ? 0 : CAJ_VK_CHUNK_DISPATCHES;
        if (s_chunk > 0 && g_vk_batch.chunkDispatches >= (unsigned) s_chunk)
            caj_vk_batch_submit_chunk();
    }
    return g_vk_batch.open;   // chunk rotation can close the batch on error
}

// ---- Eager path (v1 semantics; the profiler's per-dispatch bracket needs
// per-launch completion, so a profiled launch always lands here) -------------
static int caj_vk_launch_eager(struct caj_vk_pipe* P,
                               const int64_t* bindings,
                               const uint8_t* kinds, int n,
                               unsigned gx, unsigned gy, unsigned gz,
                               int64_t profLaunch) {
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    int ok = 0;

    // Pool sized by the distinct descriptor types actually used.
    {
        uint32_t nBuf = 0, nImg = 0, nStor = 0, nSamp = 0, nAccel = 0;
        for (int i = 0; i < n; ++i) {
            if (kinds[i] == CAJ_VKB_TEXTURE) ++nImg;
            else if (kinds[i] == CAJ_VKB_STORAGE_IMAGE) ++nStor;
            else if (kinds[i] == CAJ_VKB_SAMPLER) ++nSamp;
            else if (kinds[i] == CAJ_VKB_ACCEL) ++nAccel;
            else if (kinds[i] == CAJ_VKB_BUFFER_ARRAY)
                nBuf += CAJ_VK_BINDLESS_MAX;
            else ++nBuf;
        }
        VkDescriptorPoolSize poolSizes[5];
        uint32_t nPool = 0;
        if (nBuf) { poolSizes[nPool].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    poolSizes[nPool++].descriptorCount = nBuf; }
        if (nImg) { poolSizes[nPool].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                    poolSizes[nPool++].descriptorCount = nImg; }
        if (nStor){ poolSizes[nPool].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    poolSizes[nPool++].descriptorCount = nStor; }
        if (nSamp){ poolSizes[nPool].type = VK_DESCRIPTOR_TYPE_SAMPLER;
                    poolSizes[nPool++].descriptorCount = nSamp; }
        if (nAccel){ poolSizes[nPool].type =
                         VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
                     poolSizes[nPool++].descriptorCount = nAccel; }
        VkDescriptorPoolCreateInfo dpci;
        memset(&dpci, 0, sizeof(dpci));
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = 1;
        dpci.poolSizeCount = nPool;
        dpci.pPoolSizes = poolSizes;
        if (g_xpu_vk.vkCreateDescriptorPool(g_xpu_vk.device, &dpci, NULL,
                                            &descPool) != VK_SUCCESS)
            goto done;
    }
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    {
        VkDescriptorSetAllocateInfo dsai;
        memset(&dsai, 0, sizeof(dsai));
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = descPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &P->setLayout;
        if (g_xpu_vk.vkAllocateDescriptorSets(g_xpu_vk.device, &dsai, &descSet)
                != VK_SUCCESS) goto done;
    }
    if (!caj_vk_marshal_writes(descSet, bindings, kinds, n, /*noteBound=*/0))
        goto done;

    {
        VkCommandBufferAllocateInfo cbai;
        memset(&cbai, 0, sizeof(cbai));
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = g_xpu_vk.cmdPool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (g_xpu_vk.vkAllocateCommandBuffers(g_xpu_vk.device, &cbai, &cmd)
                != VK_SUCCESS) goto done;
    }

    // cajeta-profiler 13.2.c (spec 5.5.3-5.5.5) — bracket this dispatch with
    // timestamp queries when launched under the profiler seam. The two-slot
    // pool is reused per dispatch, which per-launch completion serializes;
    // 5.5.5 requires the reset BEFORE reuse.
    int profTiming = 0;
    if (profLaunch != 0 && __cajeta_prof_vk_timing_ok()
            && g_xpu_vk.vkCreateQueryPool && g_xpu_vk.vkCmdWriteTimestamp
            && g_xpu_vk.vkGetQueryPoolResults) {
        if (g_xpu_vk.profPoolReady == 0) {
            VkQueryPoolCreateInfo qpci;
            memset(&qpci, 0, sizeof(qpci));
            qpci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qpci.queryCount = 2;
            g_xpu_vk.profPoolReady =
                (g_xpu_vk.vkCreateQueryPool(g_xpu_vk.device, &qpci, NULL,
                                            &g_xpu_vk.profPool) == VK_SUCCESS)
                    ? 1 : -1;
        }
        profTiming = g_xpu_vk.profPoolReady > 0;
    }
#if defined(VK_VERSION_1_2)
    if (profTiming && g_xpu_vk.hasHostQueryReset)
        g_xpu_vk.vkResetQueryPool(g_xpu_vk.device, g_xpu_vk.profPool, 0, 2);
#endif
    // 6.6 — refresh the calibration when it has gone stale.
    if (profTiming) {
        const int64_t now = __cajeta_currentTimeNanos();
        if (now - g_xpu_vk.lastCalibrateNs > CAJ_VK_RECAL_INTERVAL_NS)
            caj_vk_calibrate_now(now);
    }

    {
        VkCommandBufferBeginInfo cbbi;
        memset(&cbbi, 0, sizeof(cbbi));
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        g_xpu_vk.vkBeginCommandBuffer(cmd, &cbbi);
    }
    if (profTiming && !g_xpu_vk.hasHostQueryReset)
        g_xpu_vk.vkCmdResetQueryPool(cmd, g_xpu_vk.profPool, 0, 2);
    caj_vk_record_image_transitions(cmd, bindings, kinds, n);
    g_xpu_vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                               P->pipeline);
    g_xpu_vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                     P->pipeLayout, 0, 1, &descSet, 0, NULL);
    if (profTiming)
        g_xpu_vk.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     g_xpu_vk.profPool, 0);
    g_xpu_vk.vkCmdDispatch(cmd, gx, gy, gz);
    if (profTiming) {
        // 5.5.3 — the EXPLICIT barrier before the closing timestamp: without
        // it some drivers latch the timestamp before the kernel completes.
        VkMemoryBarrier mb;
        memset(&mb, 0, sizeof(mb));
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = 0;
        g_xpu_vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                                      1, &mb, 0, NULL, 0, NULL);
        g_xpu_vk.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                     g_xpu_vk.profPool, 1);
    }
    g_xpu_vk.vkEndCommandBuffer(cmd);

    {
        VkSubmitInfo si;
        memset(&si, 0, sizeof(si));
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        if (g_xpu_vk.vkQueueSubmit(g_xpu_vk.queue, 1, &si, VK_NULL_HANDLE)
                != VK_SUCCESS) goto done;
    }
    // 5.5.8 / 14.13 — the host is about to block on the GPU; the interval is
    // an explicit span, not a gap the reader must diagnose.
    {
        const int64_t waitStart = __cajeta_currentTimeNanos();
        const VkResult wr = g_xpu_vk.vkQueueWaitIdle(g_xpu_vk.queue);
        const int64_t waitEnd = __cajeta_currentTimeNanos();
        if (profLaunch != 0)
            __cajeta_prof_vk_note_wait(0, waitStart, waitEnd);
        if (wr != VK_SUCCESS) goto done;
    }

    // 13.2.c — read the bracket by AVAILABILITY, never by value and never
    // with WAIT (5.5.4).
    if (profTiming) {
        uint64_t rr[4] = {0, 0, 0, 0};
        const VkResult qr = g_xpu_vk.vkGetQueryPoolResults(
            g_xpu_vk.device, g_xpu_vk.profPool, 0, 2, sizeof(rr), rr,
            /*stride=*/2 * sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (qr == VK_SUCCESS && rr[1] != 0 && rr[3] != 0) {
            const uint64_t startTicks = rr[0], endTicks = rr[2];
            // 5.5.7 — only history reveals a timestamp-register reset.
            const int32_t flags =
                __cajeta_prof_vk_note_span_ticks(startTicks, endTicks);
            const uint32_t bits = __cajeta_prof_vk_valid_bits();
            const uint64_t durTicks =
                __cajeta_prof_vk_delta_ticks(startTicks, endTicks, bits);
            int64_t startNs, endNs;
            if (bits >= 64 && __cajeta_prof_clock_valid(CAJ_GPU_BACKEND_VULKAN)) {
                startNs = __cajeta_prof_clock_to_host(CAJ_GPU_BACKEND_VULKAN,
                                                      (int64_t) startTicks);
                endNs = __cajeta_prof_clock_to_host(CAJ_GPU_BACKEND_VULKAN,
                                                    (int64_t) endTicks);
            } else {
                // Sub-64-bit ticks wrap inside a fit window; the DURATION is
                // still device truth, anchored at the host completion sighting.
                const double perTick =
                    __cajeta_prof_clock_valid(CAJ_GPU_BACKEND_VULKAN)
                        ? (__cajeta_prof_clock_period(CAJ_GPU_BACKEND_VULKAN)
                           * (1.0 + __cajeta_prof_clock_drift_ppm(CAJ_GPU_BACKEND_VULKAN)
                                    * 1e-6))
                        : (double) g_xpu_vk.tsPeriod;
                const int64_t durNs = (int64_t) ((double) durTicks * perTick);
                endNs = __cajeta_currentTimeNanos();
                startNs = endNs - durNs;
            }
            __cajeta_prof_vk_bracket_resolved(profLaunch, startNs, endNs,
                                              flags);
        } else {
            __cajeta_prof_vk_note_unavailable();
        }
    }
    ok = 1;

done:
    if (cmd) g_xpu_vk.vkFreeCommandBuffers(g_xpu_vk.device, g_xpu_vk.cmdPool,
                                           1, &cmd);
    if (descPool) g_xpu_vk.vkDestroyDescriptorPool(g_xpu_vk.device, descPool,
                                                   NULL);
    return ok;
}

static int cajeta_xpu_vk_launch(const void* spirv, uint64_t len,
                                const char* entry, const int64_t* bindings,
                                const uint8_t* kinds, int n,
                                unsigned gx, unsigned gy, unsigned gz,
                                unsigned bx, unsigned by, unsigned bz,
                                unsigned sharedBytes,
                                int userSpecCount,
                                const int32_t* userSpecValues) {
    if (!spirv || len < 4 || n <= 0 || n > 64) return 0;
    // CAJETA_XPU_VK_SUBMIT=eager restores the v1 submit-and-wait per launch
    // (the A/B control for the batched path; pipelines stay cached either way).
    static int eagerMode = -1;
    if (eagerMode < 0) {
        const char* m = getenv("CAJETA_XPU_VK_SUBMIT");
        eagerMode = (m && strcmp(m, "eager") == 0) ? 1 : 0;
    }
    // Serialize: VkQueue + VkCommandPool + the tables + the batch state all
    // require external host synchronization (the engine drives launches from
    // both the main and carrier-fiber threads).
    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    struct caj_vk_pipe* P = caj_vk_pipe_get(spirv, len, entry, kinds, n,
                                            bx > 0 ? bx : 1, by > 0 ? by : 1,
                                            bz > 0 ? bz : 1,
                                            sharedBytes > 0 ? sharedBytes : 0,
                                            userSpecCount, userSpecValues);
    int ok = 0;
    if (P) {
        const int64_t profLaunch = __cajeta_prof_vk_current_launch();
        if (eagerMode || profLaunch != 0) {
            // Ordering: anything already recorded must land first.
            cajeta_xpu_vk_flush();
            ok = caj_vk_launch_eager(P, bindings, kinds, n, gx, gy, gz,
                                     profLaunch);
        } else {
            ok = caj_vk_launch_batched(P, bindings, kinds, n, gx, gy, gz);
        }
    }
    if (!ok) {
        cajeta_xpu_note_launch_failure();
        fprintf(stderr, "cajeta.xpu.vulkan: launch FAILED for kernel '%s' "
                "(n=%d grid=%u,%u,%u)\n", entry ? entry : "?", n, gx, gy, gz);
        // Say WHICH binding the marshal could not resolve. A bare "launch
        // FAILED" cannot distinguish "no pipeline for this kernel" from "one
        // of the bound buffers is a dead handle", and the two have nothing in
        // common as diagnoses. Found the cajeta-llm static-field alias defect:
        // every failing dispatch named the same binding, holding a value that
        // was a slot address rather than a buffer handle.
        fprintf(stderr, "  pipe=%s", P ? "ok" : "NULL");
        for (int i = 0; i < n; ++i) {
            fprintf(stderr, "  b%d[kind=%u h=%lld rec=%s]", i,
                    (unsigned) kinds[i], (long long) bindings[i],
                    (kinds[i] == CAJ_VKB_ACCEL || kinds[i] == CAJ_VKB_TEXTURE
                     || kinds[i] == CAJ_VKB_STORAGE_IMAGE
                     || kinds[i] == CAJ_VKB_SAMPLER
                     || kinds[i] == CAJ_VKB_BUFFER_ARRAY)
                        ? "?"
                        : (cajeta_xpu_vk_rec(bindings[i]) ? "ok" : "NULL"));
        }
        fprintf(stderr, "\n");
    }
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
    return ok;
}


int32_t __cajeta_xpu_vk_built(void) { return 1; }

#else  // no Vulkan SDK header at runtime-build time — Vulkan unavailable.
static int cajeta_xpu_vulkan_init_locked(void) { return 0; }
int32_t __cajeta_xpu_vk_built(void)                 { return 0; }
int32_t __cajeta_xpu_vk_has_host_query_reset(void) { return 0; }
int32_t __cajeta_xpu_vk_has_sync2(void)            { return 0; }
int32_t __cajeta_xpu_vk_has_calibrated_ts(void)    { return 0; }
static int64_t cajeta_xpu_vk_alloc(uint64_t b) { (void) b; return 0; }
static int64_t cajeta_xpu_vk_slice(int64_t p, uint64_t o) { (void) p; (void) o; return 0; }
static void cajeta_xpu_vk_view_release(int64_t h) { (void) h; }
static void cajeta_xpu_vk_scalar_begin_launch(void) {}
static int64_t cajeta_xpu_vk_scalar_push(const void* p, uint32_t sz) {
    (void) p; (void) sz; return 0;
}
static void cajeta_xpu_vk_read(int64_t h, void* d, uint64_t b) {
    (void) h; (void) d; (void) b;
}
static void cajeta_xpu_vk_flush(void) {}
static void cajeta_xpu_vk_note_host_access(int64_t h) { (void) h; }
static void* cajeta_xpu_vk_mapped(int64_t h) { (void) h; return NULL; }
static void cajeta_xpu_vk_free(int64_t h) { (void) h; }
static int64_t cajeta_xpu_vk_tex_alloc(uint32_t w, uint32_t h, int storage,
                                       int32_t format, uint32_t depth, int imageKind,
                                       uint32_t arrayLayers, uint32_t mipLevels) {
    (void) w; (void) h; (void) storage; (void) format; (void) depth; (void) imageKind;
    (void) arrayLayers; (void) mipLevels;
    return 0;
}
static void cajeta_xpu_vk_tex_upload(int64_t h, const float* src,
                                     uint32_t w, uint32_t ht, int32_t format) {
    (void) h; (void) src; (void) w; (void) ht; (void) format;
}
static void cajeta_xpu_vk_tex_upload_level(int64_t h, const float* src,
                                           uint32_t lw, uint32_t lh,
                                           uint32_t level, int32_t format) {
    (void) h; (void) src; (void) lw; (void) lh; (void) level; (void) format;
}
static void cajeta_xpu_vk_tex_download(int64_t h, void* d,
                                       uint32_t w, uint32_t ht) {
    (void) h; (void) d; (void) w; (void) ht;
}
static void cajeta_xpu_vk_tex_free(int64_t h) { (void) h; }
static int64_t cajeta_xpu_vk_make_sampler(int32_t f, int32_t a) {
    (void) f; (void) a; return 0;
}
static void cajeta_xpu_vk_destroy_sampler(int64_t h) { (void) h; }
static int64_t cajeta_xpu_vk_accel_build_aabbs(const float* a, uint32_t c) {
    (void) a; (void) c; return 0;
}
static int64_t cajeta_xpu_vk_accel_build_triangles(const float* v, uint32_t t,
                                                   uint32_t s) {
    (void) v; (void) t; (void) s; return 0;
}
static void cajeta_xpu_vk_accel_free(int64_t h) { (void) h; }
static int cajeta_xpu_vk_launch(const void* s, uint64_t l, const char* e,
                                const int64_t* b, const uint8_t* k, int n,
                                unsigned gx, unsigned gy, unsigned gz,
                                unsigned bx, unsigned by, unsigned bz,
                                unsigned sharedBytes,
                                int userSpecCount, const int32_t* userSpecValues) {
    (void) s; (void) l; (void) e; (void) b; (void) k; (void) n;
    (void) gx; (void) gy; (void) gz; (void) bx; (void) by; (void) bz;
    (void) sharedBytes; (void) userSpecCount; (void) userSpecValues; return 0;
}
#endif  // CAJETA_RT_HAS_VULKAN

// --- registered kernel modules (cubin images keyed by PTX entry name) -------
