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
// Resolution flattens both forms into a `ResolvedFlavor` carrying
// the effective base name + the override property map. Phase 8 emits
// the overrides as compiler flags (`--flavor-<key>=<value>`); v1
// just records them so the build action can include them in the
// cache discriminator (so a custom-flavor change re-keys the cache
// even before the compiler honors the property knobs).

#pragma once

#include "cajeta/buildtool/Manifest.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <map>
#include <set>
#include <string>

namespace cajeta::buildtool {

    // The two built-in flavor names. Custom flavors must derive from
    // a built-in (their `base` field must be one of these). Cycle
    // detection between custom flavors lives in resolveFlavor.
    inline const std::set<std::string>& builtinFlavors() {
        static const std::set<std::string> s = {"release", "debug"};
        return s;
    }

    struct ResolvedFlavor {
        // The effective built-in flavor — what the compiler's
        // `--mode` flag gets.
        std::string base;
        // Property overrides composed in resolution order: custom-
        // flavor chain (deepest first) → inline map (last, wins).
        // Stored as JSON values so booleans / numbers / strings all
        // round-trip cleanly through the discriminator.
        llvm::json::Object overrides;
    };

    // Resolve a `flavor:` JSON value against the project's
    // `settings.build.custom-flavors` registry. Returns the effective
    // base + flat override map.
    //
    // Errors when:
    //   - the value is neither a string nor an object
    //   - the named custom flavor doesn't exist (and isn't a built-in)
    //   - a custom-flavor `base` chain doesn't terminate in a built-in
    //   - a custom-flavor cycle is detected (A.base=B, B.base=A)
    //   - an inline `base` is missing or isn't a string
    llvm::Expected<ResolvedFlavor> resolveFlavor(
        const llvm::json::Value& flavorRef,
        const llvm::json::Object& customFlavors);

} // namespace cajeta::buildtool
