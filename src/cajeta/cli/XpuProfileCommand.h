// `cajeta gpu-profile` — print the active GPU's DeviceProfile as one-line JSON.
#pragma once

namespace cajeta {

    // Queries and measures the device, prints the JSON, and exits the process.
    int dispatchXpuProfile(int argc, const char** argv);

} // namespace cajeta
