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

    // settings.build.binaries entry — names a buildable executable
    // with its entry method. Multi-binary projects declare several
    // here; build tasks reference them via `binary: "<name>"`.
    struct BinarySpec {
        std::string name;
        std::string entryMethod;
        std::optional<std::string> description;
    };

    // settings.build block. Carries cross-cutting build defaults the
    // build action reads when invoked.
    struct SettingsBuild {
        std::optional<std::string> entryMethod;         // single-binary default
        std::optional<std::string> target;              // "host" / target triple
        std::optional<std::string> sourceRoot;          // default: src/main/cajeta
        std::optional<std::string> outputDir;           // default: build/
        std::map<std::string, BinarySpec> binaries;     // named-binary registry
        // Phase 5b: custom-flavors map; left raw for now.
        llvm::json::Object customFlavorsRaw;
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
        // `melt` block — present only in melt packages (manifests
        // whose purpose is to export curated configuration). Mutually
        // exclusive with `tasks` and `workspace`. See cajeta/buildtool/
        // Melt.h for the typed model.
        llvm::json::Object meltRaw;
        // `workspace` block — present only in workspace-root manifests
        // (the monorepo coordination layer, Phase 12). Parses through
        // raw today; typed model lands when Phase 12 ships.
        llvm::json::Object workspaceRaw;

        // True iff the `melt` block was present at load time. Used to
        // distinguish "no melt declared" from "declared but empty".
        bool hasMelt = false;
        // True iff the `workspace` block was present at load time.
        bool hasWorkspace = false;

        // Absolute path the manifest was loaded from. Empty when the
        // manifest was loaded from an in-memory string.
        std::string sourcePath;
    };

    // Parse the `settings.build` block from a manifest. Returns a
    // typed SettingsBuild; missing block → defaults. Errors on
    // structurally invalid shapes (e.g. binaries entry without
    // `entry-method`).
    llvm::Expected<SettingsBuild> parseSettingsBuild(const Manifest& m);

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
