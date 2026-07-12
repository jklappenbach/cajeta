// The `clean` action — removes build artifacts AND the caches.
//
// Spec (BuildTool.md action catalog `clean` row):
//   Optional: keep-cache
//   Outputs:  removed-bytes, removed-entries, cache-cleaned
//
// Behavior:
//   - default            → remove `build/` and `.cajeta/cache/`.
//   - `keep-cache: true` → remove `build/` only, preserving the IR +
//                          artifact caches for a fast incremental rebuild.
//
// Clean means clean. Until 2026-07-11 the cache wipe was opt-in (`deep`) and
// prompted [y/N] on a TTY; a non-interactive spawn (the IDE's Clean) hit EOF,
// answered "no", and left the artifact cache intact — so the next build simply
// RE-PUBLISHED the cached binary in ~100ms without running the compiler, which
// presents as an instant green check under an empty phase tree.
//
// Paths are taken from `settings.build.outputDir` (defaults to
// "build") and a fixed `.cajeta/cache` location.

#include "cajeta/buildtool/Action.h"
#include "cajeta/buildtool/Manifest.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>

#include <filesystem>
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

            // `keep-cache=true` preserves .cajeta/cache/ (the IR + artifact
            // caches) — the old `deep` opt-IN, inverted.
            //
            // Clean now means clean: it wipes the caches too, unprompted. The old
            // behavior wiped only build/ unless `deep` was set, and gated the
            // cache wipe behind a [y/N] prompt that a non-interactive spawn (the
            // IDE's Clean action) answered "no" by silently hitting EOF. The next
            // build then RE-PUBLISHED the cached artifact in ~100ms without
            // running the compiler — an instant green check and an empty phase
            // tree, indistinguishable from a broken build.
            bool keepCache = false;
            if (auto v = params.getBoolean("keep-cache")) keepCache = *v;

            // Resolve the build directory from settings.build.
            std::string outputDir = "build";
            if (ctx.manifest()) {
                auto sb = parseSettingsBuild(*ctx.manifest());
                if (!sb) return sb.takeError();
                if (sb->outputDir) outputDir = *sb->outputDir;
            }

            auto buildWipe = wipeTree(fs::path(outputDir));
            if (!buildWipe) return buildWipe.takeError();

            WipeResult cacheWipe;
            if (!keepCache) {
                auto cw = wipeTree(fs::path(".cajeta") / "cache");
                if (!cw) return cw.takeError();
                cacheWipe = *cw;
            }

            ActionResult r;
            uint64_t totalEntries = buildWipe->entries + cacheWipe.entries;
            uint64_t totalBytes   = buildWipe->bytes   + cacheWipe.bytes;
            r.outputs["removed-entries"] = std::to_string(totalEntries);
            r.outputs["removed-bytes"]   = std::to_string(totalBytes);
            r.outputs["cache-cleaned"] = keepCache ? "false" : "true";
            // Say what was removed — `clean` used to print nothing at all, so
            // there was no way to tell it apart from a no-op.
            llvm::outs() << "[clean] removed " << totalEntries << " entries ("
                         << (totalBytes / 1024) << " KiB) from " << outputDir
                         << (keepCache ? "" : " and .cajeta/cache") << "\n";
            return r;
        }
    };

    std::unique_ptr<Action> makeCleanAction() {
        return std::make_unique<CleanAction>();
    }

} // namespace cajeta::buildtool
