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

#include <functional>
#include <map>
#include <optional>
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

    // --- Unit 5: resolution (probe order, version selection, override) ----

    // One required lib resolved to a concrete linker input.
    struct ResolvedNative {
        std::string lib;
        std::string version;       // concrete version chosen
        std::string platform;
        std::string artifactPath;  // provider- or override-supplied path
        std::string link;          // "static" | "dynamic"
        std::string via;           // provider name, or "override-path"
    };

    // A probe-order source of native artifacts. `supply(lib, version, platform)`
    // returns a path if this source can provide the artifact, else nullopt.
    // The resolver tries providers in order; first hit wins. Real providers
    // (.cja native/, project native/, ~/.cajeta/native cache, system, Olla
    // fetch) plug in here; tests use stubs.
    struct NativeProvider {
        std::string name;
        std::function<std::optional<std::string>(
            const std::string& lib, const std::string& version,
            const std::string& platform)> supply;
    };

    struct NativeResolution {
        std::map<std::string, ResolvedNative> resolved;
        std::map<std::string, std::string> unresolved;  // id -> reason (→ §11)
    };

    // Choose the highest-compatible version across `constraints` (concrete
    // "1.5.2" or wildcard "1.5.*"/"1.*"). Incompatible majors → error (4.A).
    llvm::Expected<std::string> selectNativeVersion(
        const std::vector<std::string>& constraints);

    // Resolve every required lib in `reqs` for `platform`: a `native-overrides`
    // entry wins (version replaces constraints; path short-circuits providers),
    // otherwise the version is selected from the constraints and providers are
    // tried in order. Unresolvable libs land in `unresolved` with a reason.
    NativeResolution resolveNatives(
        const NativeRequirementSet& reqs,
        const std::map<std::string, NativeOverride>& overrides,
        const std::string& platform,
        const std::vector<NativeProvider>& providers);

} // namespace cajeta::buildtool
