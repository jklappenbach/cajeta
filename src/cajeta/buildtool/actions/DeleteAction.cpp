// `delete` — remove files / directories. Native filesystem
// action. See BuildTool.md Action catalog "Filesystem" row.
//
// Params:
//   paths       (required) string or array of paths to remove
//   if-exists   (optional, default true) missing paths are OK;
//               set to false to error when a path doesn't exist

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

    class DeleteAction : public Action {
    public:
        std::string name() const override { return "delete"; }

        llvm::Expected<ActionResult> run(
            const llvm::json::Object& params,
            TaskContext& /*ctx*/) const override {

            std::vector<std::string> paths;
            if (auto s = params.getString("paths")) {
                paths.push_back(s->str());
            } else if (const auto* a = params.getArray("paths")) {
                for (size_t i = 0; i < a->size(); ++i) {
                    auto v = (*a)[i].getAsString();
                    if (!v) return err("delete: 'paths[" +
                                       std::to_string(i) +
                                       "]' must be a string");
                    paths.push_back(v->str());
                }
            } else {
                return err("delete: 'paths' is required "
                           "(string or array of strings)");
            }

            bool ifExists = true;
            if (auto v = params.getBoolean("if-exists")) ifExists = *v;

            namespace fs = std::filesystem;
            std::error_code ec;
            size_t removed = 0;
            for (const auto& p : paths) {
                fs::path path = p;
                if (!fs::exists(path, ec)) {
                    if (ifExists) continue;
                    return err("delete: '" + p +
                               "' does not exist (if-exists=false)");
                }
                // remove_all handles both files and directory trees.
                auto n = fs::remove_all(path, ec);
                if (ec) {
                    return err("delete: '" + p + "': " + ec.message());
                }
                removed += static_cast<size_t>(n);
            }

            ActionResult r;
            r.outputs["removed"] = std::to_string(removed);
            return r;
        }
    };

    std::unique_ptr<Action> makeDeleteAction() {
        return std::make_unique<DeleteAction>();
    }

} // namespace cajeta::buildtool
