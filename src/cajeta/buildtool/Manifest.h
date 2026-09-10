// Cajeta build-tool manifest (`cajeta.json`) data model and loader. The
// document has six top-level blocks: details, properties, settings, actions
// (presets), plugins, tasks; see BuildTool.md for the schema.

#pragma once

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    /// `details` block: package identity, on a strict schema.
    struct ManifestDetails {
        std::string name;                            // required
        std::string version;                         // required
        std::optional<std::string> description;
        std::optional<std::string> license;
        std::vector<std::string> authors;            // empty == not set
        std::optional<std::string> repositoryUrl;
        std::optional<std::string> cajetaLangVersion;
        // Plugin sidecars only; raw because the plugin protocol owns its shape.
        llvm::json::Object pluginRaw;

        std::string group() const;     // everything before last '.'
        std::string library() const;   // last segment
    };

    /// One settings.build.binaries entry: an executable and its entry method.
    struct BinarySpec {
        std::string name;
        std::string entryMethod;
        std::optional<std::string> description;
    };

    /// settings.build: the cross-cutting defaults the build action reads.
    struct SettingsBuild {
        std::optional<std::string> entryMethod;         // single-binary default
        std::optional<std::string> target;              // "host" / target triple
        std::optional<std::string> sourceRoot;          // default: src/main/cajeta
        std::optional<std::string> outputDir;           // default: build/
        std::map<std::string, BinarySpec> binaries;     // named-binary registry
        llvm::json::Object customFlavorsRaw;
        // settings.build.cache `max-bytes` / `max-age-seconds`; 0 skips the pass.
        uint64_t cacheMaxBytes = 0;
        uint64_t cacheMaxAgeSeconds = 0;
    };

    /// settings.output: where generated files go. Defaults live in the build
    /// action, so an absent key stays distinguishable from a defaulted one.
    struct SettingsOutput {
        std::optional<std::string> root;           // default: build
        std::optional<std::string> intermediates;  // default: <root>/obj
        std::optional<std::string> artifacts;      // default: <root>/archive
        std::optional<std::string> binaries;       // default: <root>/exe
    };

    /// One per-platform artifact of a settings.native-libraries entry.
    struct NativeArtifact {
        std::string platform;                 // "linux-x64", ...
        std::optional<std::string> url;       // download coordinate
        std::optional<std::string> sha256;    // integrity check
    };

    struct NativeLibrary {
        std::string id;                       // the map key
        std::string version;                  // required: default version constraint
        std::string license;                  // required: SPDX id
        bool redistributable = false;
        std::string link = "static";          // "static" | "dynamic" (default static)
        std::vector<std::string> platforms;   // supported triples
        std::map<std::string, NativeArtifact> artifacts;  // platform -> coords
        std::optional<std::string> acquire;   // embargoed acquisition instructions
    };

    /// The one developer-facing native knob: force a version or a local path.
    struct NativeOverride {
        std::optional<std::string> version;
        std::optional<std::string> path;
    };

    /// The whole document; a `*Raw` block's contents are not modeled yet.
    struct Manifest {
        ManifestDetails details;
        llvm::json::Object propertiesRaw;
        llvm::json::Object settingsRaw;
        llvm::json::Object actionsRaw;
        llvm::json::Object pluginsRaw;
        llvm::json::Object tasksRaw;
        // Melt packages only; mutually exclusive with `tasks` and `workspace`.
        llvm::json::Object meltRaw;
        // Workspace-root manifests only.
        llvm::json::Object workspaceRaw;

        // Presence flags: they separate "not declared" from "declared empty".
        bool hasMelt = false;
        bool hasWorkspace = false;

        // Empty when the manifest was loaded from an in-memory string.
        std::string sourcePath;
    };

    /// Parses `settings.build`; a missing block yields defaults.
    llvm::Expected<SettingsBuild> parseSettingsBuild(const Manifest& m);

    /// Parses and VALIDATES settings.output. Validating on load rather than
    /// at first write stops a bad value before anything is generated.
    llvm::Expected<SettingsOutput> parseSettingsOutput(const Manifest& m);

    /// Parses the entry object under one lib-id key; `where` prefixes errors.
    llvm::Expected<NativeLibrary> parseNativeLibraryEntry(
        const std::string& id, const llvm::json::Object& entry,
        const std::string& where);

    /// Parses settings.native-libraries; a missing block yields an empty map.
    llvm::Expected<std::map<std::string, NativeLibrary>>
    parseNativeLibraries(const Manifest& m);

    /// Parses settings.native-overrides; each entry must set version or path.
    llvm::Expected<std::map<std::string, NativeOverride>>
    parseNativeOverrides(const Manifest& m);

    /// Loads and validates a manifest from disk; errors are ManifestError.
    llvm::Expected<Manifest> loadManifestFile(const std::string& path);

    /// Loads a manifest from a JSONC string; `sourceLabel` names it in errors.
    llvm::Expected<Manifest> loadManifestString(
        const std::string& source,
        const std::string& sourceLabel = "<inline>");

    /// The directory holding `cajeta.json` — NOT the compile source root.
    /// Returns "." rather than "" so it is always a usable path argument.
    std::string projectRootFromManifest(const Manifest& m);

} // namespace cajeta::buildtool
