//
// Minimal Vulkan compute runtime wrapper — see header.
//

#include "VulkanDriver.h"

#if defined(__has_include)
#  if __has_include(<vulkan/vulkan.h>)
#    define CAJETA_HAS_VULKAN_HEADER 1
#  endif
#endif

#ifdef CAJETA_HAS_VULKAN_HEADER

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>

namespace cajeta {
namespace xpu {
namespace vulkan {

namespace {

void logvk(const char* what, VkResult r) {
    std::fprintf(stderr, "cajeta.xpu.vulkan: %s failed (VkResult %d)\n", what,
                 (int) r);
}

} // namespace

struct VulkanDriver::Impl {
    void* lib = nullptr;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memProps{};

    // Instance + device entry points (resolved in init()).
    PFN_vkCreateInstance createInstance = nullptr;
    PFN_vkDestroyInstance destroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices enumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties getQueueFamilyProps = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties getMemProps = nullptr;
    PFN_vkCreateDevice createDevice = nullptr;
    PFN_vkDestroyDevice destroyDevice = nullptr;
    PFN_vkGetDeviceQueue getDeviceQueue = nullptr;
    PFN_vkCreateBuffer createBuffer = nullptr;
    PFN_vkDestroyBuffer destroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements getBufferMemReq = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkBindBufferMemory bindBufferMemory = nullptr;
    PFN_vkMapMemory mapMemory = nullptr;
    PFN_vkUnmapMemory unmapMemory = nullptr;
    PFN_vkCreateShaderModule createShaderModule = nullptr;
    PFN_vkDestroyShaderModule destroyShaderModule = nullptr;
    PFN_vkCreateDescriptorSetLayout createDescSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout destroyDescSetLayout = nullptr;
    PFN_vkCreatePipelineLayout createPipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout destroyPipelineLayout = nullptr;
    PFN_vkCreateComputePipelines createComputePipelines = nullptr;
    PFN_vkDestroyPipeline destroyPipeline = nullptr;
    PFN_vkCreateDescriptorPool createDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool destroyDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets allocateDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets updateDescriptorSets = nullptr;
    PFN_vkCreateCommandPool createCommandPool = nullptr;
    PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers freeCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
    PFN_vkCmdBindPipeline cmdBindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets cmdBindDescriptorSets = nullptr;
    PFN_vkCmdDispatch cmdDispatch = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkQueueWaitIdle queueWaitIdle = nullptr;

    struct BufferRec {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
        bool live = false;
    };
    std::vector<BufferRec> buffers;  // handle = index + 1

    BufferRec* rec(VulkanDriver::Buffer h) {
        if (h == 0 || h > buffers.size()) return nullptr;
        BufferRec& r = buffers[h - 1];
        return r.live ? &r : nullptr;
    }

    template <typename F>
    void loadInstanceProc(F& fp, const char* name) {
        fp = reinterpret_cast<F>(getInstanceProcAddr(instance, name));
    }
    template <typename F>
    void loadDeviceProc(F& fp, const char* name) {
        fp = reinterpret_cast<F>(getDeviceProcAddr(device, name));
    }

    // Resolve libvulkan + instance/device, pick a compute queue, build the
    // command pool. Static (takes the Impl) so it can name the private nested
    // type; fills `d` on success.
    static bool bringUp(Impl& d);

    int findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags want) {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((typeBits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & want) == want) {
                return (int) i;
            }
        }
        return -1;
    }

    void destroy() {
        if (device) {
            for (auto& r : buffers) {
                if (!r.live) continue;
                if (r.mapped) unmapMemory(device, r.memory);
                if (r.buffer) destroyBuffer(device, r.buffer, nullptr);
                if (r.memory) freeMemory(device, r.memory, nullptr);
            }
            buffers.clear();
            if (cmdPool) destroyCommandPool(device, cmdPool, nullptr);
            destroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (instance && destroyInstance) {
            destroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }
        if (lib) { dlclose(lib); lib = nullptr; }
    }
};

bool VulkanDriver::Impl::bringUp(Impl& d) {
    // MV1: macOS loads MoltenVK (Vulkan->Metal); no native Vulkan ICD there.
#if defined(__APPLE__)
    for (const char* name : {"libvulkan.1.dylib", "libvulkan.dylib",
                             "libMoltenVK.dylib"}) {
#else
    for (const char* name : {"libvulkan.so.1", "libvulkan.so"}) {
#endif
        d.lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (d.lib) break;
    }
    if (!d.lib) return false;
    d.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(d.lib, "vkGetInstanceProcAddr"));
    if (!d.getInstanceProcAddr) return false;

    auto globalProc = [&](const char* name) {
        return d.getInstanceProcAddr(VK_NULL_HANDLE, name);
    };
    d.createInstance =
        reinterpret_cast<PFN_vkCreateInstance>(globalProc("vkCreateInstance"));
    if (!d.createInstance) return false;

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "cajeta-xpu";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkResult r = d.createInstance(&ici, nullptr, &d.instance);
    if (r != VK_SUCCESS) { logvk("vkCreateInstance", r); return false; }

    d.loadInstanceProc(d.destroyInstance, "vkDestroyInstance");
    d.loadInstanceProc(d.enumeratePhysicalDevices, "vkEnumeratePhysicalDevices");
    d.loadInstanceProc(d.getQueueFamilyProps,
                       "vkGetPhysicalDeviceQueueFamilyProperties");
    d.loadInstanceProc(d.getMemProps, "vkGetPhysicalDeviceMemoryProperties");
    d.loadInstanceProc(d.createDevice, "vkCreateDevice");
    d.loadInstanceProc(d.destroyDevice, "vkDestroyDevice");
    d.getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        d.getInstanceProcAddr(d.instance, "vkGetDeviceProcAddr"));
    if (!d.enumeratePhysicalDevices || !d.createDevice || !d.getDeviceProcAddr)
        return false;

    uint32_t count = 0;
    d.enumeratePhysicalDevices(d.instance, &count, nullptr);
    if (count == 0) return false;
    std::vector<VkPhysicalDevice> devs(count);
    d.enumeratePhysicalDevices(d.instance, &count, devs.data());

    // Pick the first device exposing a compute-capable queue family.
    bool found = false;
    for (VkPhysicalDevice pd : devs) {
        uint32_t qcount = 0;
        d.getQueueFamilyProps(pd, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qcount);
        d.getQueueFamilyProps(pd, &qcount, qprops.data());
        for (uint32_t i = 0; i < qcount; ++i) {
            if (qprops[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                d.phys = pd;
                d.queueFamily = i;
                found = true;
                break;
            }
        }
        if (found) break;
    }
    if (!found) return false;
    d.getMemProps(d.phys, &d.memProps);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = d.queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    r = d.createDevice(d.phys, &dci, nullptr, &d.device);
    if (r != VK_SUCCESS) { logvk("vkCreateDevice", r); return false; }

    d.loadDeviceProc(d.getDeviceQueue, "vkGetDeviceQueue");
    d.loadDeviceProc(d.createBuffer, "vkCreateBuffer");
    d.loadDeviceProc(d.destroyBuffer, "vkDestroyBuffer");
    d.loadDeviceProc(d.getBufferMemReq, "vkGetBufferMemoryRequirements");
    d.loadDeviceProc(d.allocateMemory, "vkAllocateMemory");
    d.loadDeviceProc(d.freeMemory, "vkFreeMemory");
    d.loadDeviceProc(d.bindBufferMemory, "vkBindBufferMemory");
    d.loadDeviceProc(d.mapMemory, "vkMapMemory");
    d.loadDeviceProc(d.unmapMemory, "vkUnmapMemory");
    d.loadDeviceProc(d.createShaderModule, "vkCreateShaderModule");
    d.loadDeviceProc(d.destroyShaderModule, "vkDestroyShaderModule");
    d.loadDeviceProc(d.createDescSetLayout, "vkCreateDescriptorSetLayout");
    d.loadDeviceProc(d.destroyDescSetLayout, "vkDestroyDescriptorSetLayout");
    d.loadDeviceProc(d.createPipelineLayout, "vkCreatePipelineLayout");
    d.loadDeviceProc(d.destroyPipelineLayout, "vkDestroyPipelineLayout");
    d.loadDeviceProc(d.createComputePipelines, "vkCreateComputePipelines");
    d.loadDeviceProc(d.destroyPipeline, "vkDestroyPipeline");
    d.loadDeviceProc(d.createDescriptorPool, "vkCreateDescriptorPool");
    d.loadDeviceProc(d.destroyDescriptorPool, "vkDestroyDescriptorPool");
    d.loadDeviceProc(d.allocateDescriptorSets, "vkAllocateDescriptorSets");
    d.loadDeviceProc(d.updateDescriptorSets, "vkUpdateDescriptorSets");
    d.loadDeviceProc(d.createCommandPool, "vkCreateCommandPool");
    d.loadDeviceProc(d.destroyCommandPool, "vkDestroyCommandPool");
    d.loadDeviceProc(d.allocateCommandBuffers, "vkAllocateCommandBuffers");
    d.loadDeviceProc(d.freeCommandBuffers, "vkFreeCommandBuffers");
    d.loadDeviceProc(d.beginCommandBuffer, "vkBeginCommandBuffer");
    d.loadDeviceProc(d.endCommandBuffer, "vkEndCommandBuffer");
    d.loadDeviceProc(d.cmdBindPipeline, "vkCmdBindPipeline");
    d.loadDeviceProc(d.cmdBindDescriptorSets, "vkCmdBindDescriptorSets");
    d.loadDeviceProc(d.cmdDispatch, "vkCmdDispatch");
    d.loadDeviceProc(d.queueSubmit, "vkQueueSubmit");
    d.loadDeviceProc(d.queueWaitIdle, "vkQueueWaitIdle");

    d.getDeviceQueue(d.device, d.queueFamily, 0, &d.queue);

    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = d.queueFamily;
    r = d.createCommandPool(d.device, &cpci, nullptr, &d.cmdPool);
    if (r != VK_SUCCESS) { logvk("vkCreateCommandPool", r); return false; }
    return true;
}

VulkanDriver::~VulkanDriver() {
    if (impl) { impl->destroy(); delete impl; impl = nullptr; }
}

bool VulkanDriver::init() {
    if (impl) return true;
    auto* d = new Impl();
    if (!Impl::bringUp(*d)) { d->destroy(); delete d; return false; }
    impl = d;
    return true;
}

bool VulkanDriver::available() {
    VulkanDriver probe;
    return probe.init();
}

bool VulkanDriver::rayQueryAvailable() {
    // Self-contained probe: load libvulkan, create a throwaway 1.3 instance,
    // and check the first compute-capable device for the ray-query extension +
    // feature set. No logical device is created. Torn down before returning.
    void* lib = nullptr;
    // MV1: macOS loads MoltenVK (Vulkan->Metal); no native Vulkan ICD there.
#if defined(__APPLE__)
    for (const char* name : {"libvulkan.1.dylib", "libvulkan.dylib",
                             "libMoltenVK.dylib"}) {
#else
    for (const char* name : {"libvulkan.so.1", "libvulkan.so"}) {
#endif
        lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (lib) break;
    }
    if (!lib) return false;
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(lib, "vkGetInstanceProcAddr"));
    if (!gipa) { dlclose(lib); return false; }

    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(
        gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!createInstance) { dlclose(lib); return false; }
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (createInstance(&ici, nullptr, &inst) != VK_SUCCESS) {
        dlclose(lib);
        return false;
    }

    auto destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
        gipa(inst, "vkDestroyInstance"));
    auto enumDevs = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        gipa(inst, "vkEnumeratePhysicalDevices"));
    auto getQueueProps =
        reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties"));
    auto enumExt = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        gipa(inst, "vkEnumerateDeviceExtensionProperties"));
    auto getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        gipa(inst, "vkGetPhysicalDeviceFeatures2"));

    bool ok = false;
    if (destroyInstance && enumDevs && getQueueProps && enumExt && getFeatures2) {
        uint32_t count = 0;
        enumDevs(inst, &count, nullptr);
        std::vector<VkPhysicalDevice> devs(count);
        if (count) enumDevs(inst, &count, devs.data());
        for (VkPhysicalDevice pd : devs) {
            // Require a compute queue (matches the device we'd actually pick).
            uint32_t qn = 0;
            getQueueProps(pd, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qp(qn);
            if (qn) getQueueProps(pd, &qn, qp.data());
            bool compute = false;
            for (auto& q : qp)
                if (q.queueFlags & VK_QUEUE_COMPUTE_BIT) { compute = true; break; }
            if (!compute) continue;

            uint32_t en = 0;
            enumExt(pd, nullptr, &en, nullptr);
            std::vector<VkExtensionProperties> exts(en);
            if (en) enumExt(pd, nullptr, &en, exts.data());
            bool hasAccel = false, hasRayQ = false, hasDefer = false, hasBDA = false;
            for (auto& e : exts) {
                if (!std::strcmp(e.extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)) hasAccel = true;
                else if (!std::strcmp(e.extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME)) hasRayQ = true;
                else if (!std::strcmp(e.extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)) hasDefer = true;
                else if (!std::strcmp(e.extensionName, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)) hasBDA = true;
            }
            if (!(hasAccel && hasRayQ && hasDefer && hasBDA)) continue;

            VkPhysicalDeviceRayQueryFeaturesKHR rqf{};
            rqf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
            VkPhysicalDeviceAccelerationStructureFeaturesKHR asf{};
            asf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
            asf.pNext = &rqf;
            VkPhysicalDeviceBufferDeviceAddressFeatures bdaf{};
            bdaf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
            bdaf.pNext = &asf;
            VkPhysicalDeviceFeatures2 f2{};
            f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            f2.pNext = &bdaf;
            getFeatures2(pd, &f2);
            if (rqf.rayQuery && asf.accelerationStructure && bdaf.bufferDeviceAddress) {
                ok = true;
                break;
            }
        }
    }
    if (destroyInstance) destroyInstance(inst, nullptr);
    dlclose(lib);
    return ok;
}

VulkanDriver::Buffer VulkanDriver::alloc(std::size_t bytes) {
    if (!impl || bytes == 0) return 0;
    Impl& d = *impl;
    Impl::BufferRec rec;
    rec.size = bytes;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = d.createBuffer(d.device, &bci, nullptr, &rec.buffer);
    if (r != VK_SUCCESS) { logvk("vkCreateBuffer", r); return 0; }

    VkMemoryRequirements req{};
    d.getBufferMemReq(d.device, rec.buffer, &req);
    int mt = d.findMemoryType(req.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) {
        std::fprintf(stderr,
                     "cajeta.xpu.vulkan: no host-visible coherent memory type\n");
        d.destroyBuffer(d.device, rec.buffer, nullptr);
        return 0;
    }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t) mt;
    r = d.allocateMemory(d.device, &mai, nullptr, &rec.memory);
    if (r != VK_SUCCESS) {
        logvk("vkAllocateMemory", r);
        d.destroyBuffer(d.device, rec.buffer, nullptr);
        return 0;
    }
    d.bindBufferMemory(d.device, rec.buffer, rec.memory, 0);
    r = d.mapMemory(d.device, rec.memory, 0, VK_WHOLE_SIZE, 0, &rec.mapped);
    if (r != VK_SUCCESS) {
        logvk("vkMapMemory", r);
        d.freeMemory(d.device, rec.memory, nullptr);
        d.destroyBuffer(d.device, rec.buffer, nullptr);
        return 0;
    }
    rec.live = true;
    d.buffers.push_back(rec);
    return (Buffer) d.buffers.size();  // handle = index + 1
}

bool VulkanDriver::upload(Buffer b, const void* src, std::size_t bytes) {
    if (!impl) return false;
    auto* r = impl->rec(b);
    if (!r || bytes > r->size) return false;
    std::memcpy(r->mapped, src, bytes);  // host-coherent: no flush needed
    return true;
}

bool VulkanDriver::download(void* dst, Buffer b, std::size_t bytes) {
    if (!impl) return false;
    auto* r = impl->rec(b);
    if (!r || bytes > r->size) return false;
    std::memcpy(dst, r->mapped, bytes);
    return true;
}

void VulkanDriver::free(Buffer b) {
    if (!impl) return;
    auto* r = impl->rec(b);
    if (!r) return;
    Impl& d = *impl;
    if (r->mapped) d.unmapMemory(d.device, r->memory);
    if (r->buffer) d.destroyBuffer(d.device, r->buffer, nullptr);
    if (r->memory) d.freeMemory(d.device, r->memory, nullptr);
    r->live = false;
    r->mapped = nullptr;
    r->buffer = VK_NULL_HANDLE;
    r->memory = VK_NULL_HANDLE;
}

bool VulkanDriver::launch(const void* spirv, std::size_t len, const char* entry,
                          const std::vector<Buffer>& bindings,
                          unsigned groupCountX) {
    if (!impl || !spirv || len < 4 || bindings.empty()) return false;
    Impl& d = *impl;
    const uint32_t n = (uint32_t) bindings.size();
    bool ok = false;

    // Transient objects, torn down at the end regardless of outcome.
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    do {
        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = len;
        smci.pCode = reinterpret_cast<const uint32_t*>(spirv);
        VkResult r = d.createShaderModule(d.device, &smci, nullptr, &module);
        if (r != VK_SUCCESS) { logvk("vkCreateShaderModule", r); break; }

        std::vector<VkDescriptorSetLayoutBinding> binds(n);
        for (uint32_t i = 0; i < n; ++i) {
            binds[i].binding = i;
            binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            binds[i].descriptorCount = 1;
            binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo slci{};
        slci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        slci.bindingCount = n;
        slci.pBindings = binds.data();
        r = d.createDescSetLayout(d.device, &slci, nullptr, &setLayout);
        if (r != VK_SUCCESS) { logvk("vkCreateDescriptorSetLayout", r); break; }

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &setLayout;
        r = d.createPipelineLayout(d.device, &plci, nullptr, &pipeLayout);
        if (r != VK_SUCCESS) { logvk("vkCreatePipelineLayout", r); break; }

        VkComputePipelineCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = module;
        cpci.stage.pName = entry;
        cpci.layout = pipeLayout;
        r = d.createComputePipelines(d.device, VK_NULL_HANDLE, 1, &cpci, nullptr,
                                     &pipeline);
        if (r != VK_SUCCESS) { logvk("vkCreateComputePipelines", r); break; }

        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = n;
        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = 1;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes = &poolSize;
        r = d.createDescriptorPool(d.device, &dpci, nullptr, &descPool);
        if (r != VK_SUCCESS) { logvk("vkCreateDescriptorPool", r); break; }

        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = descPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &setLayout;
        VkDescriptorSet descSet = VK_NULL_HANDLE;
        r = d.allocateDescriptorSets(d.device, &dsai, &descSet);
        if (r != VK_SUCCESS) { logvk("vkAllocateDescriptorSets", r); break; }

        std::vector<VkDescriptorBufferInfo> bufInfos(n);
        std::vector<VkWriteDescriptorSet> writes(n);
        bool bound = true;
        for (uint32_t i = 0; i < n; ++i) {
            auto* rec = d.rec(bindings[i]);
            if (!rec) { bound = false; break; }
            bufInfos[i].buffer = rec->buffer;
            bufInfos[i].offset = 0;
            bufInfos[i].range = VK_WHOLE_SIZE;
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = descSet;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &bufInfos[i];
        }
        if (!bound) { std::fprintf(stderr,
            "cajeta.xpu.vulkan: launch got an invalid buffer handle\n"); break; }
        d.updateDescriptorSets(d.device, n, writes.data(), 0, nullptr);

        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = d.cmdPool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        r = d.allocateCommandBuffers(d.device, &cbai, &cmd);
        if (r != VK_SUCCESS) { logvk("vkAllocateCommandBuffers", r); break; }

        VkCommandBufferBeginInfo cbbi{};
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        d.beginCommandBuffer(cmd, &cbbi);
        d.cmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        d.cmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout,
                                0, 1, &descSet, 0, nullptr);
        d.cmdDispatch(cmd, groupCountX, 1, 1);
        d.endCommandBuffer(cmd);

        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        r = d.queueSubmit(d.queue, 1, &si, VK_NULL_HANDLE);
        if (r != VK_SUCCESS) { logvk("vkQueueSubmit", r); break; }
        r = d.queueWaitIdle(d.queue);
        if (r != VK_SUCCESS) { logvk("vkQueueWaitIdle", r); break; }
        ok = true;
    } while (false);

    if (cmd) d.freeCommandBuffers(d.device, d.cmdPool, 1, &cmd);
    if (descPool) d.destroyDescriptorPool(d.device, descPool, nullptr);
    if (pipeline) d.destroyPipeline(d.device, pipeline, nullptr);
    if (pipeLayout) d.destroyPipelineLayout(d.device, pipeLayout, nullptr);
    if (setLayout) d.destroyDescSetLayout(d.device, setLayout, nullptr);
    if (module) d.destroyShaderModule(d.device, module, nullptr);
    return ok;
}

} // namespace vulkan
} // namespace xpu
} // namespace cajeta

#else  // !CAJETA_HAS_VULKAN_HEADER — no Vulkan SDK at build time.

namespace cajeta {
namespace xpu {
namespace vulkan {

struct VulkanDriver::Impl {};
VulkanDriver::~VulkanDriver() = default;
bool VulkanDriver::available() { return false; }
bool VulkanDriver::rayQueryAvailable() { return false; }
bool VulkanDriver::init() { return false; }
VulkanDriver::Buffer VulkanDriver::alloc(std::size_t) { return 0; }
bool VulkanDriver::upload(Buffer, const void*, std::size_t) { return false; }
bool VulkanDriver::download(void*, Buffer, std::size_t) { return false; }
void VulkanDriver::free(Buffer) {}
bool VulkanDriver::launch(const void*, std::size_t, const char*,
                          const std::vector<Buffer>&, unsigned) {
    return false;
}

} // namespace vulkan
} // namespace xpu
} // namespace cajeta

#endif  // CAJETA_HAS_VULKAN_HEADER
