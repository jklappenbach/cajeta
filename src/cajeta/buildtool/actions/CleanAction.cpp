// The `clean` action — removes build artifacts. Phase 5b: `deep`
// param extends the wipe to `.cajeta/cache/` (the IR cache).
//
// Spec (BuildTool.md action catalog `clean` row):
//   Optional: deep, yes
//   Outputs:  removed-bytes, removed-entries
//
// Behavior:
//   - `deep: false` (default) → remove `build/`.
//   - `deep: true`            → also remove `.cajeta/cache/`.
//   - `yes: true`             → skip the TTY confirmation prompt
//                               for the deep wipe (CI scripts).
//                               No prompt on a non-deep clean —
//                               wiping `build/` is unsurprising.
//
// Paths are taken from `settings.build.outputDir` (defaults to
// "build") and a fixed `.cajeta/cache` location.

#include "cajeta/buildtool/Action.h"
#include "cajeta/buildtool/Manifest.h"

#include <llvm/Support/Error.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        // Count + remove a directory tree. Returns (entries, bytes)
        // so the action can report what was reclaimed.
        struct WipeResult {
            uint64_t entries = 0;
            uint64_t bytes = 0;
        };
        llvm::Expected<WipeResult> wipeTree(
            const std::filesystem::path& root) {
            namespace fs = std::filesystem;
            WipeResult r;
            std::error_code ec;
            if (!fs::exists(root, ec)) {
                return r;
            }
            for (auto it = fs::recursive_directory_iterator(root, ec);
                 !ec && it != fs::recursive_directory_iterator(); ++it) {
                if (it->is_regular_file()) {
                    ++r.entries;
                    std::error_code se;
                    r.bytes += fs::file_size(it->path(), se);
                }
            }
            std::error_code rmEc;
            fs::remove_all(root, rmEc);
            if (rmEc) {
                return err("clean: cannot remove '" + root.string() +
                           "': " + rmEc.message());
            }
            return r;
        }

    } // namespace

    class CleanAction : public Action {
    public:
        std::string name() const override { return "clean"; }

        llvm::Expected<ActionResult> run(
            const llvm::json::Object& params,
            TaskContext& ctx) const override {
            namespace fs = std::filesystem;

            bool deep = false;
            if (auto v = params.getBoolean("deep")) deep = *v;
            bool assumeYes = false;
            if (auto v = params.getBoolean("yes")) assumeYes = *v;

            // Resolve the build directory from settings.build.
            std::string outputDir = "build";
            if (ctx.manifest()) {
                auto sb = parseSettingsBuild(*ctx.manifest());
                if (!sb) return sb.takeError();
                if (sb->outputDir) outputDir = *sb->outputDir;
            }

            // Always wipe build/.
            auto buildWipe = wipeTree(fs::path(outputDir));
            if (!buildWipe) return buildWipe.takeError();

            WipeResult cacheWipe;
            if (deep) {
                // Prompt before wiping the cache when interactive
                // and the consumer didn't pre-approve via `yes`.
                // Same posture as `cajeta coverage remove`.
                if (!assumeYes && ::isatty(STDIN_FILENO) != 0) {
                    std::cout << "Wipe .cajeta/cache/ (IR cache)? [y/N] ";
                    std::string line;
                    std::getline(std::cin, line);
                    if (line.empty() ||
                        (line[0] != 'y' && line[0] != 'Y')) {
                        std::cout << "skipping cache wipe.\n";
                    } else {
                        auto cw = wipeTree(fs::path(".cajeta") / "cache");
                        if (!cw) return cw.takeError();
                        cacheWipe = *cw;
                    }
                } else {
                    auto cw = wipeTree(fs::path(".cajeta") / "cache");
                    if (!cw) return cw.takeError();
                    cacheWipe = *cw;
                }
            }

            ActionResult r;
            uint64_t totalEntries = buildWipe->entries + cacheWipe.entries;
            uint64_t totalBytes   = buildWipe->bytes   + cacheWipe.bytes;
            r.outputs["removed-entries"] = std::to_string(totalEntries);
            r.outputs["removed-bytes"]   = std::to_string(totalBytes);
            r.outputs["deep"] = deep ? "true" : "false";
            return r;
        }
    };

    std::unique_ptr<Action> makeCleanAction() {
        return std::make_unique<CleanAction>();
    }

} // namespace cajeta::buildtool
