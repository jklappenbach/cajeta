// Typed wrapper over the CMake-embedded `cajeta init` archetype templates.
// `samples/buildtool/<type>/` is the single source of truth; the CMake glob embeds only
// `cajeta.json` and the `.cajeta` sources under `src/`.

#pragma once

#include <llvm/Support/Error.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cajeta::buildtool {

    struct InitTemplateFile {
        // Path relative to the new project root, forward-slash separated.
        std::string relativePath;
        std::string_view contents;
    };

    struct InitTemplate {
        std::string name;
        std::vector<InitTemplateFile> files;
    };

    // Names of all embedded archetypes, in CMake declaration order (stable for completion).
    std::vector<std::string> availableInitTemplates();

    // The named template, or nullopt when no archetype of that exact name was embedded.
    std::optional<InitTemplate> findInitTemplate(std::string_view name);

    struct InitWriteResult {
        // Paths created, relative to destDir, in template-declaration order.
        std::vector<std::string> filesWritten;
    };

    // Write the named template under `destDir`, creating parent directories. Errors when
    // the template is unknown, `destDir` exists as a non-directory, or a target file exists
    // and `force` is false - refusing up front beats a half-written tree.
    llvm::Expected<InitWriteResult> instantiateInitTemplate(
        std::string_view templateName,
        const std::string& destDir,
        bool force);

} // namespace cajeta::buildtool
