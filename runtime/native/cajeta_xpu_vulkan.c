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
};
#define CAJETA_VK_MAX_BUFFERS 4096
static struct cajeta_vk_buf g_vk_bufs[CAJETA_VK_MAX_BUFFERS];
static int g_vk_buf_count;

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
        for (uint32_t qi = 0; qi < qn; ++qi) {
            if (qp[qi].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                g_xpu_vk.phys = devs[di];
                g_xpu_vk.queueFamily = qi;
                found = 1;
                break;
            }
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
    int wantInt8 = 0, wantInt64 = 0;
    if (getFeatures2) {
        VkPhysicalDeviceShaderFloat16Int8Features qi8;
        memset(&qi8, 0, sizeof(qi8));
        qi8.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
        VkPhysicalDeviceFeatures2 qf2;
        memset(&qf2, 0, sizeof(qf2));
        qf2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        qf2.pNext = &qi8;
        getFeatures2(g_xpu_vk.phys, &qf2);
        wantInt8 = qi8.shaderInt8 ? 1 : 0;
        wantInt64 = qf2.features.shaderInt64 ? 1 : 0;
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
        dci.enabledExtensionCount = nDevExts;
        dci.ppEnabledExtensionNames = devExts;
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
    // shaderInt64 is a CORE feature -> pEnabledFeatures (legal alongside the pNext
    // extension-feature chain, which carries no VkPhysicalDeviceFeatures2).
    VkPhysicalDeviceFeatures coreFeats;
    memset(&coreFeats, 0, sizeof(coreFeats));
    if (wantInt64) {
        coreFeats.shaderInt64 = VK_TRUE;
        dci.pEnabledFeatures = &coreFeats;
    }

    if (g_xpu_vk.vkCreateDevice(g_xpu_vk.phys, &dci, NULL, &g_xpu_vk.device)
            != VK_SUCCESS)
        return 0;

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
    g_xpu_vk.loaded = 1;
    return 1;
}

// Allocate a host-visible/coherent storage buffer; return a 1-based table
// handle (0 on failure). Reuses a dead slot if one is free.
static int64_t cajeta_xpu_vk_alloc(uint64_t bytes) {
    if (bytes == 0) return 0;
    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf = VK_NULL_HANDLE;
    if (g_xpu_vk.vkCreateBuffer(g_xpu_vk.device, &bci, NULL, &buf) != VK_SUCCESS)
        return 0;
    VkMemoryRequirements req;
    memset(&req, 0, sizeof(req));
    g_xpu_vk.vkGetBufferMemoryRequirements(g_xpu_vk.device, buf, &req);
    int mt = cajeta_xpu_vk_find_memory_type(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) { g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, buf, NULL); return 0; }
    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t) mt;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (g_xpu_vk.vkAllocateMemory(g_xpu_vk.device, &mai, NULL, &mem)
            != VK_SUCCESS) {
        g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, buf, NULL);
        return 0;
    }
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
    int slot = -1;
    for (int i = 0; i < g_vk_buf_count; ++i)
        if (!g_vk_bufs[i].live) { slot = i; break; }
    if (slot < 0) {
        if (g_vk_buf_count >= CAJETA_VK_MAX_BUFFERS) {
            pthread_mutex_unlock(&g_xpu_vk_submit_mu);
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
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
    return (int64_t) (slot + 1);
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
    int slot = -1;
    for (int i = 0; i < g_vk_buf_count; ++i)
        if (!g_vk_bufs[i].live) { slot = i; break; }
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
static void cajeta_xpu_vk_free(int64_t handle) {
    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    struct cajeta_vk_buf* r = cajeta_xpu_vk_rec(handle);
    if (r) {
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
    int64_t staging = cajeta_xpu_vk_alloc(bytes);   // host-visible+coherent
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
    g_xpu_vk.vkDestroySampler(g_xpu_vk.device,
                              (VkSampler) (uintptr_t) handle, NULL);
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

static int cajeta_xpu_vk_launch(const void* spirv, uint64_t len,
                                const char* entry, const int64_t* bindings,
                                const uint8_t* kinds, int n,
                                unsigned gx, unsigned gy, unsigned gz,
                                unsigned bx, unsigned by, unsigned bz,
                                unsigned sharedBytes,
                                int userSpecCount,
                                const int32_t* userSpecValues) {
    if (!spirv || len < 4 || n <= 0) return 0;
    // Serialize the dispatch: VkQueue + VkCommandPool require external host
    // synchronization, and an AS binding reads the g_vk_accels table — all shared
    // with the build/free paths, which may run on a different OS thread.
    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    int ok = 0;

    VkShaderModuleCreateInfo smci;
    memset(&smci, 0, sizeof(smci));
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = (size_t) len;
    smci.pCode = (const uint32_t*) spirv;
    if (g_xpu_vk.vkCreateShaderModule(g_xpu_vk.device, &smci, NULL, &module)
            != VK_SUCCESS) goto done;

    VkDescriptorSetLayoutBinding binds[64];
    if (n > 64) goto done;
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
                                             &setLayout) != VK_SUCCESS) goto done;

    VkPipelineLayoutCreateInfo plci;
    memset(&plci, 0, sizeof(plci));
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &setLayout;
    if (g_xpu_vk.vkCreatePipelineLayout(g_xpu_vk.device, &plci, NULL,
                                        &pipeLayout) != VK_SUCCESS) goto done;

    // Spec constants: SpecId 0/1/2 = block.x/y/z (workgroup size, see
    // injectWorkgroupSizeSpecConstant), SpecId 3 = the dynamic shared array's
    // length in elements (= sharedBytes / 4; the dynamic-shared element is 4 bytes
    // — int32/float32 — for now). SpecId kFirstUserSpecId(4)+i = the user's
    // Spec.geti/getf slot `i`, supplied as a host override (raw 4-byte word; i32
    // today, f32 reinterpreted later). All set at pipeline creation.
    enum { CAJ_VK_FIRST_USER_SPEC_ID = 4 };   // mirrors LoweringTarget::kFirstUserSpecId
    int nUser = userSpecCount;
    if (nUser < 0) nUser = 0;
    if (nUser > 60) nUser = 60;               // cap: 4 reserved + 60 user <= 64
    uint32_t specData[64];
    VkSpecializationMapEntry specEntries[64];
    specData[0] = bx; specData[1] = by; specData[2] = bz;
    specData[3] = sharedBytes / 4u;
    specEntries[0] = (VkSpecializationMapEntry){ 0, 0,                    sizeof(uint32_t) };
    specEntries[1] = (VkSpecializationMapEntry){ 1, sizeof(uint32_t),     sizeof(uint32_t) };
    specEntries[2] = (VkSpecializationMapEntry){ 2, 2 * sizeof(uint32_t), sizeof(uint32_t) };
    specEntries[3] = (VkSpecializationMapEntry){ 3, 3 * sizeof(uint32_t), sizeof(uint32_t) };
    for (int i = 0; i < nUser; ++i) {
        specData[4 + i] = (uint32_t) userSpecValues[i];   // raw word (bit-exact)
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
    cpci.stage.module = module;
    cpci.stage.pName = entry;
    cpci.stage.pSpecializationInfo = &specInfo;
    cpci.layout = pipeLayout;
    if (g_xpu_vk.vkCreateComputePipelines(g_xpu_vk.device, VK_NULL_HANDLE, 1,
                                          &cpci, NULL, &pipeline) != VK_SUCCESS)
        goto done;

    // Pool sized by the distinct descriptor types actually used (storage
    // buffer / sampled image / sampler), one entry per non-empty class.
    uint32_t nBuf = 0, nImg = 0, nStor = 0, nSamp = 0, nAccel = 0;
    for (int i = 0; i < n; ++i) {
        if (kinds[i] == CAJ_VKB_TEXTURE) ++nImg;
        else if (kinds[i] == CAJ_VKB_STORAGE_IMAGE) ++nStor;
        else if (kinds[i] == CAJ_VKB_SAMPLER) ++nSamp;
        else if (kinds[i] == CAJ_VKB_ACCEL) ++nAccel;
        else if (kinds[i] == CAJ_VKB_BUFFER_ARRAY) nBuf += CAJ_VK_BINDLESS_MAX;
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
    if (nAccel){ poolSizes[nPool].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
                 poolSizes[nPool++].descriptorCount = nAccel; }
    VkDescriptorPoolCreateInfo dpci;
    memset(&dpci, 0, sizeof(dpci));
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = nPool;
    dpci.pPoolSizes = poolSizes;
    if (g_xpu_vk.vkCreateDescriptorPool(g_xpu_vk.device, &dpci, NULL, &descPool)
            != VK_SUCCESS) goto done;

    VkDescriptorSetAllocateInfo dsai;
    memset(&dsai, 0, sizeof(dsai));
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = descPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &setLayout;
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    if (g_xpu_vk.vkAllocateDescriptorSets(g_xpu_vk.device, &dsai, &descSet)
            != VK_SUCCESS) goto done;

    VkDescriptorBufferInfo bufInfos[64];
    // Per-array-binding descriptor infos (CAJ_VK_BINDLESS_MAX each). One row per
    // binding index; only buffer-array bindings use their row.
    VkDescriptorBufferInfo arrInfos[64 * CAJ_VK_BINDLESS_MAX];
    VkDescriptorImageInfo imgInfos[64];
    VkWriteDescriptorSet writes[64];
    // Acceleration-structure writes chain their AS handle in via pNext; both the
    // pNext struct and the handle it points at must outlive vkUpdateDescriptorSets.
    VkWriteDescriptorSetAccelerationStructureKHR accelInfos[64];
    VkAccelerationStructureKHR accelHandles[64];
    memset(writes, 0, sizeof(writes[0]) * n);
    memset(imgInfos, 0, sizeof(imgInfos[0]) * n);
    memset(accelInfos, 0, sizeof(accelInfos[0]) * n);
    for (int i = 0; i < n; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descSet;
        writes[i].dstBinding = (uint32_t) i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = cajeta_vkb_desc_type(kinds[i]);
        if (kinds[i] == CAJ_VKB_ACCEL) {
            struct cajeta_vk_accel* a = cajeta_xpu_vk_accel_rec(bindings[i]);
            if (!a) goto done;
            accelHandles[i] = a->accel;
            accelInfos[i].sType =
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
            accelInfos[i].accelerationStructureCount = 1;
            accelInfos[i].pAccelerationStructures = &accelHandles[i];
            writes[i].pNext = &accelInfos[i];
        } else if (kinds[i] == CAJ_VKB_TEXTURE) {
            struct cajeta_vk_tex* t = cajeta_xpu_vk_tex_rec(bindings[i]);
            if (!t) goto done;
            imgInfos[i].imageView = t->view;
            imgInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            writes[i].pImageInfo = &imgInfos[i];
        } else if (kinds[i] == CAJ_VKB_STORAGE_IMAGE) {
            // Writable storage image (Image2D): bound in GENERAL layout, the only
            // layout valid for an OpImageWrite descriptor. The pre-dispatch
            // barrier below transitions the image into GENERAL.
            struct cajeta_vk_tex* t = cajeta_xpu_vk_tex_rec(bindings[i]);
            if (!t) goto done;
            imgInfos[i].imageView = t->view;
            imgInfos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            writes[i].pImageInfo = &imgInfos[i];
        } else if (kinds[i] == CAJ_VKB_SAMPLER) {
            imgInfos[i].sampler = (VkSampler) (uintptr_t) bindings[i];
            if (imgInfos[i].sampler == VK_NULL_HANDLE) goto done;
            writes[i].pImageInfo = &imgInfos[i];
        } else if (kinds[i] == CAJ_VKB_BUFFER_ARRAY) {
            // bindings[i] points at the launch-marshalled [int64 count, int64
            // h0 … h(count-1)]. Bind CAJ_VK_BINDLESS_MAX descriptors: the first
            // `count` real, the rest padded with handle[0] (a valid buffer) so
            // every descriptor in the array is bound (avoids PARTIALLY_BOUND).
            // The kernel only reads bufs[0..count).
            const int64_t* arr = (const int64_t*) (intptr_t) bindings[i];
            int64_t cnt = arr ? arr[0] : 0;
            if (cnt < 1 || cnt > CAJ_VK_BINDLESS_MAX) goto done;
            VkDescriptorBufferInfo* row = &arrInfos[i * CAJ_VK_BINDLESS_MAX];
            for (int e = 0; e < CAJ_VK_BINDLESS_MAX; ++e) {
                int64_t h = arr[1 + (e < (int) cnt ? e : 0)];   // pad with elem 0
                struct cajeta_vk_buf* r = cajeta_xpu_vk_rec(h);
                if (!r) goto done;
                row[e].buffer = r->buffer;
                row[e].offset = r->view_offset;
                row[e].range = VK_WHOLE_SIZE;
            }
            writes[i].descriptorCount = CAJ_VK_BINDLESS_MAX;
            writes[i].pBufferInfo = row;
        } else {
            struct cajeta_vk_buf* r = cajeta_xpu_vk_rec(bindings[i]);
            if (!r) goto done;
            bufInfos[i].buffer = r->buffer;
            bufInfos[i].offset = r->view_offset;   // 0 for an owner; slice byte offset for a view
            bufInfos[i].range = VK_WHOLE_SIZE;
            writes[i].pBufferInfo = &bufInfos[i];
        }
    }
    g_xpu_vk.vkUpdateDescriptorSets(g_xpu_vk.device, (uint32_t) n, writes, 0,
                                    NULL);

    VkCommandBufferAllocateInfo cbai;
    memset(&cbai, 0, sizeof(cbai));
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = g_xpu_vk.cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (g_xpu_vk.vkAllocateCommandBuffers(g_xpu_vk.device, &cbai, &cmd)
            != VK_SUCCESS) goto done;

    VkCommandBufferBeginInfo cbbi;
    memset(&cbbi, 0, sizeof(cbbi));
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g_xpu_vk.vkBeginCommandBuffer(cmd, &cbbi);
    // Storage images (Image2D) must be in GENERAL layout for OpImageWrite /
    // OpImageRead. Barrier each before binding the pipeline. The barrier is
    // emitted even when the image is ALREADY GENERAL (a prior dispatch): then it
    // is not a layout transition but a read/write-after-write memory dependency,
    // so a kernel that loads what an earlier dispatch stored sees the new texels
    // (img.load reading a previous dispatch's img.store).
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
        // From GENERAL: a prior dispatch's shader writes must be made available
        // before this dispatch's shader read/write. From UNDEFINED/other: a plain
        // transition with no prior shader access to wait on.
        toGen.srcAccessMask = wasGeneral ? VK_ACCESS_SHADER_WRITE_BIT : 0;
        toGen.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        g_xpu_vk.vkCmdPipelineBarrier(
            cmd,
            wasGeneral ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                       : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
            0, NULL, 0, NULL, 1, &toGen);
        t->layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    g_xpu_vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    g_xpu_vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                     pipeLayout, 0, 1, &descSet, 0, NULL);
    g_xpu_vk.vkCmdDispatch(cmd, gx, gy, gz);   // 3-D grid (block dim is baked)
    g_xpu_vk.vkEndCommandBuffer(cmd);

    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (g_xpu_vk.vkQueueSubmit(g_xpu_vk.queue, 1, &si, VK_NULL_HANDLE)
            != VK_SUCCESS) goto done;
    if (g_xpu_vk.vkQueueWaitIdle(g_xpu_vk.queue) != VK_SUCCESS) goto done;
    ok = 1;

done:
    if (cmd) g_xpu_vk.vkFreeCommandBuffers(g_xpu_vk.device, g_xpu_vk.cmdPool, 1,
                                           &cmd);
    if (descPool) g_xpu_vk.vkDestroyDescriptorPool(g_xpu_vk.device, descPool,
                                                   NULL);
    if (pipeline) g_xpu_vk.vkDestroyPipeline(g_xpu_vk.device, pipeline, NULL);
    if (pipeLayout) g_xpu_vk.vkDestroyPipelineLayout(g_xpu_vk.device, pipeLayout,
                                                     NULL);
    if (setLayout) g_xpu_vk.vkDestroyDescriptorSetLayout(g_xpu_vk.device,
                                                         setLayout, NULL);
    if (module) g_xpu_vk.vkDestroyShaderModule(g_xpu_vk.device, module, NULL);
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
    return ok;
}

#else  // no Vulkan SDK header at runtime-build time — Vulkan unavailable.
static int cajeta_xpu_vulkan_init_locked(void) { return 0; }
static int64_t cajeta_xpu_vk_alloc(uint64_t b) { (void) b; return 0; }
static int64_t cajeta_xpu_vk_slice(int64_t p, uint64_t o) { (void) p; (void) o; return 0; }
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
