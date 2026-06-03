// `copy` — copy a file or directory tree from `from` to `to`.
// Phase 4 native filesystem action. See BuildTool.md Action
// catalog "Filesystem" row.
//
// Params:
//   from        (required) source path (file or directory)
//   to          (required) destination path
//   also        (optional) array of additional source paths copied
//                          alongside the primary one (Phase 9 uses
//                          this for `.sig` next to `.cja`)
//   mkdir       (optional, default true) create dest parent dirs
//   recursive   (optional, default true) recurse into source dirs

#include "cajeta/buildtool/Action.h"

#include <llvm/Support/Error.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace cajeta::buildtool {

    namespace {
        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }
    }

    class CopyAction : public Action {
    public:
        std::string name() const override { return "copy"; }

        llvm::Expected<ActionResult> run(
            const llvm::json::Object& params,
            TaskContext& /*ctx*/) const override {

            auto from = params.getString("from");
            auto to   = params.getString("to");
            if (!from) return err("copy: missing required 'from'");
            if (!to)   return err("copy: missing required 'to'");

            std::vector<std::string> sources;
            sources.push_back(from->str());
            if (const auto* a = params.getArray("also")) {
                for (size_t i = 0; i < a->size(); ++i) {
                    auto s = (*a)[i].getAsString();
                    if (!s) return err("copy: 'also[" + std::to_string(i) +
                                       "]' must be a string");
                    sources.push_back(s->str());
                }
            }

            bool mkdirFlag = true;
            if (auto v = params.getBoolean("mkdir")) mkdirFlag = *v;
            bool recursive = true;
            if (auto v = params.getBoolean("recursive")) recursive = *v;

            namespace fs = std::filesystem;
            std::error_code ec;

            fs::path dest = to->str();
            // If the destination ends with `/` or is an existing
            // directory, treat it as a directory; otherwise it's a
            // file target and `from` is renamed to it.
            bool destIsDir = (!dest.empty() && dest.string().back() == '/')
                          || fs::is_directory(dest, ec);
            if (destIsDir && mkdirFlag) {
                fs::create_directories(dest, ec);
                if (ec) return err("copy: cannot create '" + dest.string() +
                                   "': " + ec.message());
            } else if (mkdirFlag && dest.has_parent_path()) {
                fs::create_directories(dest.parent_path(), ec);
                if (ec) return err("copy: cannot create parent of '" +
                                   dest.string() + "': " + ec.message());
            }

            std::vector<std::string> destinations;
            destinations.reserve(sources.size());

            auto copyOpts = recursive
                ? fs::copy_options::recursive | fs::copy_options::overwrite_existing
                : fs::copy_options::overwrite_existing;

            for (const auto& src : sources) {
                fs::path srcPath = src;
                if (!fs::exists(srcPath, ec)) {
                    return err("copy: source does not exist: '" + src + "'");
                }
                fs::path target = destIsDir
                    ? (dest / srcPath.filename())
                    : dest;
                fs::copy(srcPath, target, copyOpts, ec);
                if (ec) {
                    return err("copy: '" + src + "' -> '" + target.string() +
                               "': " + ec.message());
                }
                destinations.push_back(target.string());
            }

            ActionResult r;
            // Single dest: expose as `path`. Always expose the array
            // form as `destinations` for callers that handle the
            // multi-file case.
            r.outputs["path"] = destinations.front();
            // Join destinations with newlines for the `destinations`
            // field; consumers needing structured data should rely on
            // `path` + their own list construction for now.
            std::string joined;
            for (size_t i = 0; i < destinations.size(); ++i) {
                if (i) joined += "\n";
                joined += destinations[i];
            }
            r.outputs["destinations"] = joined;
            r.outputs["count"] = std::to_string(destinations.size());
            return r;
        }
    };

    std::unique_ptr<Action> makeCopyAction() {
        return std::make_unique<CopyAction>();
    }

} // namespace cajeta::buildtool
