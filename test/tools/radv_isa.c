// cajeta_radv_isa <module.spv>: build one SPIR-V compute module into a pipeline
// on whatever Vulkan device the loader offers, for a test that runs it under
// RADV's null device (VK_DRIVER_FILES=<radeon icd>, RADV_FORCE_FAMILY=gfx1151,
// RADV_DEBUG=shaders) and reads the ISA RADV prints to stderr. No GPU is used.
// The entry point is the module's first OpEntryPoint. Loads libvulkan at run
// time, so building it needs only the headers. Exit 0 when the pipeline built,
// 2 on a usage or file error, 3 when Vulkan or a device is missing, 1 otherwise.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOAD(inst, name) PFN_##name name = (PFN_##name) gipa(inst, #name)
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "radv_isa: %s -> %d\n", #x, r_); return 1; } } while (0)

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: cajeta_radv_isa module.spv\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 20 || n % 4) { fclose(f); return 2; }
    uint32_t* code = malloc((size_t) n);
    size_t got = fread(code, 1, (size_t) n, f);
    fclose(f);
    if (got != (size_t) n || code[0] != 0x07230203u) return 2;
    // The first OpEntryPoint (opcode 15): model, id, then the name.
    const char* entry = NULL;
    for (long i = 5; i < n / 4;) {
        uint32_t wc = code[i] >> 16, op = code[i] & 0xFFFFu;
        if (wc == 0) break;
        if (op == 15u && wc >= 4) { entry = (const char*) &code[i + 3]; break; }
        i += wc;
    }
    if (!entry) return 2;
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "radv_isa: no libvulkan\n"); return 3; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr) dlsym(lib, "vkGetInstanceProcAddr");
    if (!gipa) return 3;
    LOAD(NULL, vkCreateInstance);
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { fprintf(stderr, "radv_isa: no instance\n"); return 3; }
    LOAD(inst, vkEnumeratePhysicalDevices);
    LOAD(inst, vkGetPhysicalDeviceProperties);
    LOAD(inst, vkGetPhysicalDeviceFeatures2);
    LOAD(inst, vkCreateDevice);
    LOAD(inst, vkGetDeviceProcAddr);
    uint32_t pc = 1;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &pc, &pd) < 0 || pc == 0) { fprintf(stderr, "radv_isa: no device\n"); return 3; }
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(pd, &pp);
    fprintf(stderr, "radv_isa device: %s\n", pp.deviceName);
    // Every feature the device has, so the module's capabilities are enabled.
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2,
                              .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
    VkDevice dev;
    CK(vkCreateDevice(pd, &dci, NULL, &dev));
    PFN_vkCreateShaderModule vkCreateShaderModule = (PFN_vkCreateShaderModule) vkGetDeviceProcAddr(dev, "vkCreateShaderModule");
    PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout = (PFN_vkCreateDescriptorSetLayout) vkGetDeviceProcAddr(dev, "vkCreateDescriptorSetLayout");
    PFN_vkCreatePipelineLayout vkCreatePipelineLayout = (PFN_vkCreatePipelineLayout) vkGetDeviceProcAddr(dev, "vkCreatePipelineLayout");
    PFN_vkCreateComputePipelines vkCreateComputePipelines = (PFN_vkCreateComputePipelines) vkGetDeviceProcAddr(dev, "vkCreateComputePipelines");
    VkShaderModuleCreateInfo si = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t) n, .pCode = code};
    VkShaderModule sm;
    CK(vkCreateShaderModule(dev, &si, NULL, &sm));
    // Set 0, storage buffers at every binding a kernel's parameters can take.
    VkDescriptorSetLayoutBinding b[32];
    for (int i = 0; i < 32; ++i)
        b[i] = (VkDescriptorSetLayoutBinding) {(uint32_t) i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                               VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = 32, .pBindings = b};
    VkDescriptorSetLayout dsl;
    CK(vkCreateDescriptorSetLayout(dev, &li, NULL, &dsl));
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = 1, .pSetLayouts = &dsl};
    VkPipelineLayout pl;
    CK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
    VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = entry},
        .layout = pl};
    VkPipeline p;
    CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &p));
    fprintf(stderr, "radv_isa: pipeline built\n");
    return 0;
}
