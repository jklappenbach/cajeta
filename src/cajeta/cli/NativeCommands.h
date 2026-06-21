// `cajeta fetch` / `cajeta vendor` — native-dependency provisioning CLI
// (native-deps unit 14). Thin driver surface over the unit-9 provisioning
// functions (fetchNativeToCache / vendorNativeArtifact).

#pragma once

namespace cajeta {
    // Handles `cajeta fetch ...` and `cajeta vendor ...` (verb in argv[1]).
    int dispatchNative(int argc, const char* argv[]);
}
