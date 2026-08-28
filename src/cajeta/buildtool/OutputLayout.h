// Where a build's generated files go — the one resolution both the build
// action and `cajeta artifact-path` read.
//
// This module exists because unit 4 needs the SAME answer the build gives.
// A discovery verb that recomputes the layout is a second implementation of
// it, and the moment the two drift the verb reports a path nothing writes to
// — which is worse than the hard-coded `ls -t build/archive/*.cja` it
// replaces, because that at least fails loudly when the file is absent.
// So the resolution lives here once, BuildAction.cpp calls it to decide
// where to write, and the verb calls it to say where that was.
//
// Contents follow the chain a caller actually walks:
//   resolveOutputLayout   — the four roots (spec §3.1 defaults, §3.3 overrides)
//   resolveEntryMethod    — the action's effective entry method
//   resolveEmitMode       — emit param, or the default implied by the above
//   resolveArtifactLocation — emit + roots -> the artifact's path
//
// `resolveEntryMethod` sits here rather than with the build action because
// the DEFAULT emit depends on it (an entry method means an executable), and
// emit is what picks the artifact's home. Splitting them would put half of
// one decision on each side of the drift this file exists to prevent.

#pragma once

#include "cajeta/buildtool/Manifest.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <filesystem>
#include <string>

namespace cajeta::buildtool {

    // The four output roots, fully resolved. Relative paths stay relative to
    // the project root — the caller makes them absolute if it needs that, so
    // the build tool keeps emitting the short paths its logs have always had.
    struct OutputLayout {
        std::filesystem::path root;           // build
        std::filesystem::path intermediates;  // <root>/obj
        std::filesystem::path artifacts;      // <root>/archive
        std::filesystem::path binaries;       // <root>/exe
    };

    // Resolve the roots from a manifest. `settings.build.output-dir` is
    // honoured as the legacy spelling of `output.root`; `settings.output`
    // wins when both are set, being newer and more specific. A null manifest
    // yields the defaults, which is what an unmanifested invocation gets.
    //
    // Propagates settings.output's load-time validation (spec §4.5), so a
    // bad value stops the caller before anything is generated.
    llvm::Expected<OutputLayout> resolveOutputLayout(const Manifest* manifest);

    // The build action's effective entry method, by the four-step precedence:
    // the `entry-method` param, the `binary` param resolved against
    // settings.build.binaries, then settings.build.entry-method. Returns the
    // empty string when none resolves — that is not an error here, because
    // whether it is one depends on the emit mode.
    llvm::Expected<std::string> resolveEntryMethod(
        const llvm::json::Object& params, const Manifest* manifest);

    // The emit mode a build action produces: an explicit `emit` param, else
    // "executable" when an entry method resolves and "archived-ir" when none
    // does. Rejects an unrecognized `emit`.
    llvm::Expected<std::string> resolveEmitMode(
        const llvm::json::Object& params, const Manifest* manifest);

    // Where one build action's deliverable lands.
    struct ArtifactLocation {
        std::string emit;                       // the normalized emit mode
        std::filesystem::path archiveRoot;      // the artifact's home directory
        std::filesystem::path intermediates;    // the compiler's third positional
        // The artifact itself. EMPTY for exploded-ir, whose deliverable is the
        // emitted tree rather than a single file — callers that need one path
        // must treat that as "this project has no single artifact".
        std::filesystem::path path;
    };

    // Map an emit mode onto the layout. `detailsName` / `version` name the
    // artifact: `<name>-<version>.cja` for a library, `<name>` for an
    // executable.
    //
    // archiveRoot and intermediates are the same path for exploded-ir alone,
    // and deliberately: there the emitted IR tree IS the deliverable, so its
    // artifact home and its output directory are legitimately one directory.
    // Everywhere else they are separated — an exe build that left its objects
    // beside the binary is what made a `details.name` equal to a top-level
    // package name unlinkable.
    llvm::Expected<ArtifactLocation> resolveArtifactLocation(
        const OutputLayout& layout, llvm::StringRef emit,
        llvm::StringRef detailsName, llvm::StringRef version);

} // namespace cajeta::buildtool
