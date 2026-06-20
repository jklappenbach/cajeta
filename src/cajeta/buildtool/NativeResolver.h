// Native-dependency resolver (native-deps units 4–5).
//
// Unit 4: the requirement model + transitive collection — read each .cja's
// embedded native metadata (native/native-libraries.json) and union by lib-id,
// flagging required-but-unprovided libs. Unit 5 adds probe-order resolution +
// version selection on top of this set.
//
// See docs/specification/buildtool/native-deps-spec.md §4.

#pragma once

#include "Manifest.h"
#include "cajeta/compile/CajetaArchive.h"

#include <llvm/Support/Error.h>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // One .cja's embedded native metadata (native/native-libraries.json):
    //   { "requires": ["zstd", …],            // lib-ids the code @Native-binds
    //     "libraries": { "zstd": {…}, … } }   // resolution metadata
    struct NativeMeta {
        std::set<std::string> requiredLibs;
        std::map<std::string, NativeLibrary> libraries;
    };

    // The transitive union across a dependency set.
    struct NativeRequirementSet {
        std::set<std::string> required;                  // every lib-id needed
        std::map<std::string, NativeLibrary> libraries;  // id -> resolution metadata
        std::set<std::string> unsatisfied;               // required, no metadata anywhere
        // id -> the version constraints seen across deps (resolved in unit 5).
        std::map<std::string, std::vector<std::string>> versionConstraints;
    };

    // Parse one .cja's embedded native metadata json.
    llvm::Expected<NativeMeta> parseNativeMeta(
        llvm::StringRef json, const std::string& sourceLabel);

    // Collect + union native requirements across a set of archives (an app's
    // transitive .cja deps). Order-independent. A required lib-id with no
    // resolution metadata anywhere lands in `unsatisfied` (fail-loud, §11).
    llvm::Expected<NativeRequirementSet> collectNativeRequirements(
        const std::vector<const cajeta::CajetaArchive*>& archives);

} // namespace cajeta::buildtool
