#include "cajeta/cli/XpuProfileCommand.h"

#include "cajeta/xpu/core/DeviceProfile.h"

#include <cstdlib>
#include <iostream>

namespace cajeta {

// Prints the live device profile as JSON, then exits the process directly: the
// GPU driver's dlopened LLVM collides with the fork LLVM's cl::opt dtors at exit.
int dispatchXpuProfile(int /*argc*/, const char** /*argv*/) {
    auto profile = cajeta::xpu::queryLiveDeviceProfile();
    std::cout << cajeta::xpu::formatDeviceProfileJson(profile) << std::endl;
    std::_Exit(0);
}

} // namespace cajeta
