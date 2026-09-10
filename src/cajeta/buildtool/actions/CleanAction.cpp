// The `clean` action: removes `build/` and `.cajeta/cache/`, or with `keep-cache: true`
// only `build/`, preserving the IR and artifact caches for a fast incremental rebuild.
// Reports removed-bytes, removed-entries and cache-cleaned.

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

        // Remove a directory tree, returning the entries and bytes reclaimed so the
        // action can report them.
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

            // Clean means clean: the caches go too, unprompted. `keep-cache=true` is the
            // opt-out that keeps .cajeta/cache/, the IR and artifact caches.
            bool keepCache = false;
            if (auto v = params.getBoolean("keep-cache")) keepCache = *v;

            // Resolve from settings.build, then let settings.output override it. Clean MUST
            // follow the build action's resolution, or it wipes a directory nothing writes to.
            std::string outputDir = "build";
            SettingsOutput so;
            if (ctx.manifest()) {
                auto sb = parseSettingsBuild(*ctx.manifest());
                if (!sb) return sb.takeError();
                if (sb->outputDir) outputDir = *sb->outputDir;
                auto parsed = parseSettingsOutput(*ctx.manifest());
                if (!parsed) return parsed.takeError();
                so = std::move(*parsed);
            }
            if (so.root) outputDir = *so.root;

            auto buildWipe = wipeTree(fs::path(outputDir));
            if (!buildWipe) return buildWipe.takeError();

            // A key pointed OUTSIDE the root is still this project's output, so clean owns
            // it; skipped when already under the root, so the common case wipes one tree.
            std::vector<std::string> extraRootsWiped;
            {
                fs::path rootPath = fs::path(outputDir).lexically_normal();
                for (const auto* p : {&so.intermediates, &so.artifacts,
                                      &so.binaries}) {
                    if (!*p) continue;
                    fs::path candidate = fs::path(**p).lexically_normal();
                    auto rel = candidate.lexically_relative(rootPath);
                    // Compare the first COMPONENT, not a string prefix: path::native() is
                    // wstring on Windows, where a narrow compare() does not even compile,
                    // and a prefix test would also misread a sibling named "..foo".
                    bool insideRoot = !rel.empty() && *rel.begin() != "..";
                    if (insideRoot) continue;
                    auto w = wipeTree(candidate);
                    if (!w) return w.takeError();
                    buildWipe->entries += w->entries;
                    buildWipe->bytes   += w->bytes;
                    extraRootsWiped.push_back(candidate.string());
                }
            }

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
            // Name EVERY root actually wiped: reporting only `build` would hide the
            // deletion of a tree settings.output pointed outside it.
            std::string roots = outputDir;
            for (const auto& extra : extraRootsWiped) roots += ", " + extra;
            if (!keepCache) roots += " and .cajeta/cache";
            llvm::outs() << "[clean] removed " << totalEntries << " entries ("
                         << (totalBytes / 1024) << " KiB) from " << roots
                         << "\n";
            return r;
        }
    };

    std::unique_ptr<Action> makeCleanAction() {
        return std::make_unique<CleanAction>();
    }

} // namespace cajeta::buildtool
