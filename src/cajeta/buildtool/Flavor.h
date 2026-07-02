// Flavor model + composition.
//
// A `flavor:` entry on a build action is either:
//
//   1. A string — names a built-in (`release`, `debug`) OR a
//      project-defined custom flavor declared in
//      `settings.build.custom-flavors.<name>`.
//   2. A map  — inline composition: `{ "base": "release",
//      "debug-info": "full", "lto": true }`.
//
// Phase 5b shipped the resolver shape; Phase 8 adds:
//   - Property vocabulary (typed key/value table from BuildTool.md
//     "Property vocabulary").
//   - Load-time validation: unknown property keys, custom-flavor
//     cycles, and bad base references are hard errors at
//     manifest-load (loadManifestString hooks this in).
//   - Built-in property bundles for `release` + `debug`.
//   - Effective-property materialisation: built-in defaults +
//     chain overrides + inline overrides, in resolution order.
//   - Compiler-flag emission so BuildAction can pass the resolved
//     map through to the compiler.

#pragma once


#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <set>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // The two built-in flavor names. Custom flavors must derive from
    // a built-in (their `base` field must transitively resolve to one).
    inline const std::set<std::string>& builtinFlavors() {
        static const std::set<std::string> s = {"release", "debug"};
        return s;
    }

    // Property vocabulary entry — see BuildTool.md "Property
    // vocabulary" for the source-of-truth table.
    struct FlavorPropertySpec {
        std::string key;
        // EnumStringCsv: a comma-separated list whose every token must be in
        // `allowed` (e.g. xpu-backend="amdgpu,vulkan,cpu" bundles several device
        // targets in one build); lowers to the raw string, which the frontend
        // CLI splits on commas.
        // FreeString: any non-empty string, lowered verbatim (e.g. xpu-arch=
        // gfx1151 / sm_89 / vulkan1.3 — an open device-arch set the build tool
        // does not enumerate).
        enum class Kind { Boolean, EnumString, EnumStringCsv, FreeString };
        Kind kind;
        // Populated when kind == EnumString / EnumStringCsv; the closed set of
        // accepted strings (per token for EnumStringCsv).
        std::vector<std::string> allowed;
        // The compiler CLI flag this property lowers to (without the
        // leading `--`), e.g. "bounds" for the "bounds-check" property.
        // Empty when the property is build-flavor intent with no compiler
        // frontend flag — sanitizers, lto, strip-symbols, debug-info — which
        // are honored (or reserved) at the emit/link stage, not passed to the
        // frontend. `toCompilerFlags` emits only the mapped ones.
        std::string compilerFlag;
    };

    // The full vocabulary. Indexed lookup goes through
    // findFlavorPropertySpec; this is the canonical listing the
    // error-citation builder consults.
    const std::vector<FlavorPropertySpec>& flavorPropertyVocab();

    // Returns nullptr when the key isn't in the vocabulary.
    const FlavorPropertySpec* findFlavorPropertySpec(llvm::StringRef key);

    // Property bundle for one of the two built-ins. Errors when
    // `name` isn't a built-in.
    llvm::Expected<llvm::json::Object> builtinFlavorProperties(
        llvm::StringRef name);

    // Validate a (key, value) pair against the vocabulary. The error
    // message carries `where` (e.g. "custom-flavors.integration" or
    // "inline flavor") so the user can find the offending entry.
    llvm::Error validateFlavorProperty(
        llvm::StringRef key,
        const llvm::json::Value& value,
        llvm::StringRef where);

    // Walk `settings.build.custom-flavors` at manifest-load time:
    //   - every override key must be in the vocabulary (typos error)
    //   - every value must match the property's type/enum
    //   - every `base` chain must terminate in a built-in
    //   - cycles in the chain are rejected
    // Wired into loadManifestString.
    llvm::Error validateCustomFlavors(
        const llvm::json::Object& customFlavors);

    struct ResolvedFlavor {
        // The effective built-in flavor — what the compiler's
        // `--mode` flag gets.
        std::string base;
        // Property overrides composed in resolution order: custom-
        // flavor chain (deepest first) → inline map (last, wins).
        llvm::json::Object overrides;
    };

    // Resolve a `flavor:` JSON value against the project's
    // `settings.build.custom-flavors` registry. Returns the effective
    // base + flat override map. Also re-validates every override key
    // and value (so inline-form callers get the same key-vocab
    // enforcement as the load-time walk).
    llvm::Expected<ResolvedFlavor> resolveFlavor(
        const llvm::json::Value& flavorRef,
        const llvm::json::Object& customFlavors);

    // Materialise the full property map for a resolved flavor:
    // built-in defaults overlaid with `r.overrides`. This is what
    // the compiler sees.
    llvm::Expected<llvm::json::Object> effectiveProperties(
        const ResolvedFlavor& r);

    // Emit `--<key>=<value>` flags from a property map. Booleans
    // render as "true"/"false"; strings render as their raw value.
    // Keys are emitted in vocabulary order so the argv string is
    // deterministic (so the build-cache discriminator is stable
    // across runs).
    std::vector<std::string> toCompilerFlags(
        const llvm::json::Object& props);

} // namespace cajeta::buildtool
