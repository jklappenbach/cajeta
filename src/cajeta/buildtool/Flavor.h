// Flavor model: a build action's `flavor:` is a built-in name, a custom-flavor
// name, or an inline `{base, ...properties}` map. Vocabulary + resolver.

#pragma once


#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <set>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // The built-in flavors; every custom flavor's `base` chain must end in one.
    inline const std::set<std::string>& builtinFlavors() {
        static const std::set<std::string> s = {"release", "debug"};
        return s;
    }

    // One entry of the property vocabulary (BuildTool.md "Property vocabulary").
    struct FlavorPropertySpec {
        std::string key;
        // EnumStringCsv: comma-separated, every token in `allowed`, lowered as
        // the raw string. FreeString: any non-empty string, lowered verbatim.
        enum class Kind { Boolean, EnumString, EnumStringCsv, FreeString };
        Kind kind;
        // The closed set of accepted strings; empty unless kind is an enum.
        std::vector<std::string> allowed;
        // Compiler CLI flag without the leading `--`, empty when the property
        // is honored at the emit/link stage instead; only mapped keys lower.
        std::string compilerFlag;
    };

    // The canonical listing, in lowering order; lookup by key goes through
    // findFlavorPropertySpec.
    const std::vector<FlavorPropertySpec>& flavorPropertyVocab();

    // Returns nullptr when the key isn't in the vocabulary.
    const FlavorPropertySpec* findFlavorPropertySpec(llvm::StringRef key);

    // The default property bundle of a built-in; errors on any other name.
    llvm::Expected<llvm::json::Object> builtinFlavorProperties(
        llvm::StringRef name);

    // Checks one (key, value) against the vocabulary. `where` is the site
    // quoted back to the user, e.g. "custom-flavors.integration".
    llvm::Error validateFlavorProperty(
        llvm::StringRef key,
        const llvm::json::Value& value,
        llvm::StringRef where);

    // Rejects unknown keys, mistyped values, non-built-in base chains and cycles
    // across `settings.build.custom-flavors`; called from loadManifestString.
    llvm::Error validateCustomFlavors(
        const llvm::json::Object& customFlavors);

    // `base` is the built-in the compiler's `--mode` gets; `overrides` is the
    // flat property map, custom-flavor chain first and the inline map last.
    struct ResolvedFlavor {
        std::string base;
        llvm::json::Object overrides;
    };

    // Resolves a `flavor:` value against the custom-flavor registry, re-validating
    // every override so inline callers meet the load-time walk's enforcement.
    llvm::Expected<ResolvedFlavor> resolveFlavor(
        const llvm::json::Value& flavorRef,
        const llvm::json::Object& customFlavors);

    // The full map the compiler sees: built-in defaults under `r.overrides`.
    llvm::Expected<llvm::json::Object> effectiveProperties(
        const ResolvedFlavor& r);

    // Emits `--<key>=<value>` for each mapped property, in vocabulary order so
    // the argv (and the build-cache discriminator it feeds) is deterministic.
    std::vector<std::string> toCompilerFlags(
        const llvm::json::Object& props);

} // namespace cajeta::buildtool
