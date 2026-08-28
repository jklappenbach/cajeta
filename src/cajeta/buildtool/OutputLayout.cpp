#include "cajeta/buildtool/OutputLayout.h"

#include <llvm/Support/Error.h>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

    } // namespace

    llvm::Expected<OutputLayout> resolveOutputLayout(const Manifest* manifest) {
        namespace fs = std::filesystem;

        std::string outputDir = "build";
        SettingsOutput cfg;
        if (manifest) {
            auto sb = parseSettingsBuild(*manifest);
            if (!sb) return sb.takeError();
            if (sb->outputDir) outputDir = *sb->outputDir;
            auto parsed = parseSettingsOutput(*manifest);
            if (!parsed) return parsed.takeError();
            cfg = std::move(*parsed);
        }
        if (cfg.root) outputDir = *cfg.root;

        OutputLayout l;
        l.root = fs::path(outputDir);
        l.intermediates = cfg.intermediates ? fs::path(*cfg.intermediates)
                                            : l.root / "obj";
        l.artifacts = cfg.artifacts ? fs::path(*cfg.artifacts)
                                    : l.root / "archive";
        // <root>/exe, not §3.1's build/bin: unit 2 kept the executable where
        // the toolchain skill and check-guide-part1.sh expect it. The KEY is
        // `binaries`, so adopting bin later is a default change rather than a
        // new setting.
        l.binaries = cfg.binaries ? fs::path(*cfg.binaries) : l.root / "exe";
        return l;
    }

    llvm::Expected<std::string> resolveEntryMethod(
        const llvm::json::Object& params, const Manifest* manifest) {
        if (auto v = params.getString("entry-method")) {
            return v->str();
        }
        if (auto v = params.getString("binary")) {
            if (!manifest) {
                return err("build: 'binary' param requires a manifest "
                           "(settings.build.binaries lookup); none "
                           "provided to the runner");
            }
            auto sb = parseSettingsBuild(*manifest);
            if (!sb) return sb.takeError();
            auto it = sb->binaries.find(v->str());
            if (it == sb->binaries.end()) {
                std::string available;
                for (const auto& kv : sb->binaries) {
                    if (!available.empty()) available += ", ";
                    available += kv.first;
                }
                return err("build: 'binary' '" + v->str() +
                           "' not found in settings.build.binaries "
                           "(available: " +
                           (available.empty() ? "<none>" : available) + ")");
            }
            return it->second.entryMethod;
        }
        if (manifest) {
            auto sb = parseSettingsBuild(*manifest);
            if (!sb) return sb.takeError();
            if (sb->entryMethod) return *sb->entryMethod;
        }
        return std::string("");
    }

    llvm::Expected<std::string> resolveEmitMode(
        const llvm::json::Object& params, const Manifest* manifest) {
        if (auto v = params.getString("emit")) {
            std::string emit = v->str();
            if (emit != "exploded-ir" && emit != "archived-ir" &&
                emit != "executable") {
                return err("build: 'emit' must be one of "
                           "exploded-ir / archived-ir / executable; got '" +
                           emit + "'");
            }
            return emit;
        }
        auto entry = resolveEntryMethod(params, manifest);
        if (!entry) return entry.takeError();
        return std::string(entry->empty() ? "archived-ir" : "executable");
    }

    llvm::Expected<ArtifactLocation> resolveArtifactLocation(
        const OutputLayout& layout, llvm::StringRef emit,
        llvm::StringRef detailsName, llvm::StringRef version) {
        namespace fs = std::filesystem;

        ArtifactLocation loc;
        loc.emit = emit.str();
        if (emit == "exploded-ir") {
            loc.archiveRoot = layout.root / "ir";
            loc.intermediates = loc.archiveRoot;
            // `path` stays empty: the deliverable is the tree, not a file.
        } else if (emit == "archived-ir") {
            loc.archiveRoot = layout.artifacts;
            loc.intermediates = layout.intermediates;
            loc.path = loc.archiveRoot /
                       (detailsName.str() + "-" + version.str() + ".cja");
        } else if (emit == "executable") {
            loc.archiveRoot = layout.binaries;
            loc.intermediates = layout.intermediates;
            loc.path = loc.archiveRoot / detailsName.str();
        } else {
            return err("build: 'emit' must be one of "
                       "exploded-ir / archived-ir / executable; got '" +
                       emit.str() + "'");
        }
        return loc;
    }

} // namespace cajeta::buildtool
