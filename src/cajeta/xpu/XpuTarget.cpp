//
// XPU backend dispatch — see header.
//

#include "XpuTarget.h"

#include "nvidia/NvptxRegistration.h"
#include "amd/AmdgpuRegistration.h"
#include "vulkan/VulkanRegistration.h"

namespace cajeta {
namespace xpu {

    int emitKernelRegistration(Backend backend,
                               const std::vector<MethodPtr>& kernels,
                               llvm::Module& hostModule,
                               const std::string& arch) {
        switch (backend) {
            case Backend::Nvptx:
                return nvidia::emitKernelRegistration(kernels, hostModule, arch);
            case Backend::Amdgpu:
                return amd::emitKernelRegistration(kernels, hostModule, arch);
            case Backend::Spirv:
                return vulkan::emitKernelRegistration(kernels, hostModule, arch);
        }
        return 0;
    }

} // namespace xpu
} // namespace cajeta
