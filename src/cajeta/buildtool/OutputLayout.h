// Where a build's generated files go — the one resolution both the build action and
// `cajeta artifact-path` read, so a discovery verb can never report a path nothing
// writes to. `resolveEntryMethod` lives here because the default emit depends on it.

#pragma once

#include "cajeta/buildtool/Manifest.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <filesystem>
#include <string>

namespace cajeta::buildtool {

    // The four output roots, fully resolved. Relative paths stay relative to the
    // project root, so the build tool keeps emitting the short paths its logs show.
    struct OutputLayout {
        std::filesystem::path root;           // build
        std::filesystem::path intermediates;  // <root>/obj
        std::filesystem::path artifacts;      // <root>/archive
        std::filesystem::path binaries;       // <root>/exe
    };

    // Resolve the roots from a manifest, a null one yielding the defaults. Legacy
    // `settings.build.output-dir` is honoured, but `settings.output` wins over it, and
    // its load-time validation propagates so a bad value stops the caller early.
    llvm::Expected<OutputLayout> resolveOutputLayout(const Manifest* manifest);

    // The effective entry method, by precedence: the `entry-method` param, the `binary`
    // param against settings.build.binaries, then settings.build.entry-method. Empty is
    // not an error here: whether it is one depends on the emit mode.
    llvm::Expected<std::string> resolveEntryMethod(
        const llvm::json::Object& params, const Manifest* manifest);

    // The emit mode: an explicit `emit` param, else "executable" when an entry method
    // resolves and "archived-ir" when none does. Rejects an unrecognized `emit`.
    llvm::Expected<std::string> resolveEmitMode(
        const llvm::json::Object& params, const Manifest* manifest);

    // Where one build action's deliverable lands.
    struct ArtifactLocation {
        std::string emit;                       // the normalized emit mode
        std::filesystem::path archiveRoot;      // the artifact's home directory
        std::filesystem::path intermediates;    // the compiler's third positional
        // EMPTY for exploded-ir, whose deliverable is the emitted tree, not one file.
        std::filesystem::path path;
    };

    // Map an emit mode onto the layout; `detailsName`/`version` name the artifact,
    // `<name>-<version>.cja` for a library and `<name>` for an executable. archiveRoot
    // and intermediates coincide for exploded-ir alone, where the tree is the artifact.
    llvm::Expected<ArtifactLocation> resolveArtifactLocation(
        const OutputLayout& layout, llvm::StringRef emit,
        llvm::StringRef detailsName, llvm::StringRef version);

} // namespace cajeta::buildtool
