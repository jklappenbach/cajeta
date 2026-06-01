// Cajeta build-tool manifest (`cajeta.json`) data model and loader.
//
// The manifest has six top-level blocks: details, properties, settings,
// actions (presets), plugins, tasks. See BuildTool.md "Manifest —
// cajeta.json" for the spec. Phase 0 scope (plan/build-tool-plan.md):
// load the document, validate top-level structure + the details block,
// preserve unknown fields in other blocks for forward compatibility.
// Subsequent phases extend Settings/Tasks/Actions/Plugins/Properties
// modeling.

#pragma once

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <optional>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // `details` block — package identity. Phase 0 models the required
    // fields plus the commonly-set optional ones. Unknown fields are
    // currently rejected (strict schema for `details`); extending the
    // schema requires a doc + schema-version bump.
    struct ManifestDetails {
        std::string name;                            // required
        std::string version;                         // required
        std::optional<std::string> description;
        std::optional<std::string> license;
        std::vector<std::string> authors;            // empty == not set
        std::optional<std::string> repositoryUrl;
        std::optional<std::string> cajetaLangVersion;

        // Derived from `name` by splitting on the last `.`. Cached at
        // load time so consumers don't recompute on every reference.
        std::string group() const;     // everything before last '.'
        std::string library() const;   // last segment
    };

    // Top-level manifest. Phase 0 keeps the non-details blocks as raw
    // JSON values so we can validate they exist and have the right
    // top-level type without yet modeling their contents. Later phases
    // replace each `*Raw` with a typed model.
    struct Manifest {
        ManifestDetails details;
        llvm::json::Object propertiesRaw;
        llvm::json::Object settingsRaw;
        llvm::json::Object actionsRaw;
        llvm::json::Object pluginsRaw;
        llvm::json::Object tasksRaw;

        // Absolute path the manifest was loaded from. Empty when the
        // manifest was loaded from an in-memory string.
        std::string sourcePath;
    };

    // Load + validate a manifest from disk. Errors are
    // `cajeta::buildtool::ManifestError` with citation-style messages.
    llvm::Expected<Manifest> loadManifestFile(const std::string& path);

    // Load + validate a manifest from an in-memory JSONC string.
    // `sourceLabel` is used in error messages (a filename or
    // "<inline>" or similar).
    llvm::Expected<Manifest> loadManifestString(
        const std::string& source,
        const std::string& sourceLabel = "<inline>");

} // namespace cajeta::buildtool
