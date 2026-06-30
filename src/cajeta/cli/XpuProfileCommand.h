//
// `cajeta gpu-profile` — interrogate the active GPU and print its DeviceProfile
// as one-line JSON (xpu-device-profile U3). env-capture.sh consumes it into the
// profile suite's env.csv. Nothing is persisted.
//

#pragma once

namespace cajeta {

    // Run `cajeta gpu-profile`: query + measure the device, print JSON, return 0.
    int dispatchXpuProfile(int argc, const char** argv);

} // namespace cajeta
