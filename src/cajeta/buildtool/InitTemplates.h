// InitTemplates.h — typed wrapper over the CMake-embedded
// `cajeta init` archetype templates.
//
// `samples/buildtool/<type>/` is the single source of truth: the
// directory trees there are the worked examples documented in
// cajeta-docs/BuildTool.md AND the byte source for what the init
// command writes. EmbedInitTemplates.cmake compiles those trees
// into cajeta_init_embedded.cpp; this header exposes them through
// a typed API.
//
// Convention enforced by the CMake glob: only `cajeta.json` and
// `.cajeta` source files under `src/` are embedded. `run.sh` /
// `README.md` describe the sample's place in the repo, not what a
// freshly-init'd project needs — see the init command for the
// "next steps" message it prints in lieu of a README.

#pragma once

#include <llvm/Support/Error.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cajeta::buildtool {

    struct InitTemplateFile {
        // Path relative to the new project root. Forward-slash
        // separated (the on-disk samples ship that way; the writer
        // converts to native separators only where needed — Linux
        // and macOS take them as-is).
        std::string relativePath;
        // File contents — embedded verbatim from the sample tree.
        std::string_view contents;
    };

    struct InitTemplate {
        std::string name;
        std::vector<InitTemplateFile> files;
    };

    // Names of all embedded archetypes, in the declaration order
    // CMake passes them (currently: basic, workspace, multi-binary,
    // melt). Stable enough that CLI completion can rely on it.
    std::vector<std::string> availableInitTemplates();

    // Returns the named template, or nullopt if no archetype with
    // that name was embedded. Comparison is exact and
    // case-sensitive.
    std::optional<InitTemplate> findInitTemplate(std::string_view name);

    struct InitWriteResult {
        // Paths the writer created, relative to destDir, in writer
        // order (template-declaration order).
        std::vector<std::string> filesWritten;
    };

    // Writes the named template under `destDir`. Creates `destDir`
    // and any required parent directories. Errors if the template
    // doesn't exist, if `destDir` exists as a non-directory, or if
    // any target file already exists and `force` is false.
    //
    // The pre-flight existence check is intentional: a partial
    // write that aborted halfway would leave a confusing mix of
    // template + pre-existing files. Refusing up front means the
    // user can either pick a different directory or pass --force
    // deliberately.
    llvm::Expected<InitWriteResult> instantiateInitTemplate(
        std::string_view templateName,
        const std::string& destDir,
        bool force);

} // namespace cajeta::buildtool
