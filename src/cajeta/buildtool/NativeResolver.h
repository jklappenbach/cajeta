// Native-dependency resolver: requirement model, transitive collection, probe-order
// resolution and packaging. See docs/specification/buildtool/native-deps-spec.md §4.
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

    // One .cja's embedded native metadata: { requires: [lib-id...], libraries: {id: meta} }.
    struct NativeMeta {
        std::set<std::string> requiredLibs;
        std::map<std::string, NativeLibrary> libraries;
    };

    struct NativeRequirementSet {
        std::set<std::string> required;                  // every lib-id needed
        std::map<std::string, NativeLibrary> libraries;  // id -> resolution metadata
        std::set<std::string> unsatisfied;               // required, no metadata anywhere
        std::map<std::string, std::vector<std::string>> versionConstraints;
    };

    llvm::Expected<NativeMeta> parseNativeMeta(
        llvm::StringRef json, const std::string& sourceLabel);

    // Union native requirements across transitive .cja deps; unmet ids go to `unsatisfied`.
    llvm::Expected<NativeRequirementSet> collectNativeRequirements(
        const std::vector<const cajeta::CajetaArchive*>& archives);

    // --- Unit 5: resolution (probe order, version selection, override) ----

    struct ResolvedNative {
        std::string lib;
        std::string version;       // concrete version chosen
        std::string platform;
        std::string artifactPath;  // provider- or override-supplied path
        std::string link;          // "static" | "dynamic"
        std::string via;           // provider name, or "override-path"
    };

    // A probe-order source of native artifacts; providers are tried in order, first hit wins.
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

    // Highest-compatible version across `constraints`; incompatible majors are an error.
    llvm::Expected<std::string> selectNativeVersion(
        const std::vector<std::string>& constraints);

    // Resolve every required lib in `reqs` for `platform`: a `native-overrides` entry wins
    // and short-circuits providers, else version selection then providers in order.
    NativeResolution resolveNatives(
        const NativeRequirementSet& reqs,
        const std::map<std::string, NativeOverride>& overrides,
        const std::string& platform,
        const std::vector<NativeProvider>& providers);

    // --- Unit 6: default packaging step -----------------------------------

    // Serialize a requirement set to the embedded metadata json parseNativeMeta reads.
    std::string serializeNativeMeta(const NativeRequirementSet& reqs);

    struct NativePackagingResult {
        std::vector<std::string> baked;    // "<platform>/<lib>" entries baked
        std::vector<std::string> missing;  // required libs unresolved everywhere
    };

    // Bake resolved native artifacts into `arc`'s native/ tree and embed the metadata json;
    // `slim` skips baking. Baking is redistributable-agnostic - that flag gates only sharing.
    llvm::Expected<NativePackagingResult> bakeNativeArtifacts(
        cajeta::CajetaArchive& arc,
        const NativeRequirementSet& reqs,
        const std::map<std::string, NativeResolution>& perPlatform,
        bool slim);

} // namespace cajeta::buildtool
