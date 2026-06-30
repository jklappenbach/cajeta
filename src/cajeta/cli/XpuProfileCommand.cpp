//
// `cajeta gpu-profile` — see header.
//

#include "cajeta/cli/XpuProfileCommand.h"

#include "cajeta/xpu/core/DeviceProfile.h"

#include <cstdlib>
#include <iostream>

namespace cajeta {

int dispatchXpuProfile(int /*argc*/, const char** /*argv*/) {
    auto profile = cajeta::xpu::queryLiveDeviceProfile();
    std::cout << cajeta::xpu::formatDeviceProfileJson(profile) << std::endl;
    // Touching the GPU dlopens the driver's LLVM, which collides with the fork
    // LLVM's static cl::opt dtors at exit (the test-main _Exit story). Skip them.
    std::_Exit(0);
}

} // namespace cajeta
