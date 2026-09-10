// `cajeta fetch` / `cajeta vendor` — the native-dependency provisioning CLI.

#pragma once

namespace cajeta {
    // Handles `cajeta fetch ...` and `cajeta vendor ...` (verb in argv[1]).
    int dispatchNative(int argc, const char* argv[]);
}
