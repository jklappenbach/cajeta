// `mkdir` — create a directory. Native filesystem action.
//
// Params:
//   path        (required) directory path to create
//   recursive   (optional, default true) create parents as needed

#include "cajeta/buildtool/Action.h"

#include <llvm/Support/Error.h>

#include <filesystem>
#include <string>
#include <system_error>

namespace cajeta::buildtool {

    namespace {
        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }
    }

    class MkdirAction : public Action {
    public:
        std::string name() const override { return "mkdir"; }

        llvm::Expected<ActionResult> run(
            const llvm::json::Object& params,
            TaskContext& /*ctx*/) const override {

            auto path = params.getString("path");
            if (!path) return err("mkdir: missing required 'path'");
            bool recursive = true;
            if (auto v = params.getBoolean("recursive")) recursive = *v;

            namespace fs = std::filesystem;
            std::error_code ec;
            bool created = false;
            if (recursive) {
                created = fs::create_directories(path->str(), ec);
            } else {
                created = fs::create_directory(path->str(), ec);
            }
            if (ec) {
                return err("mkdir: '" + path->str() + "': " + ec.message());
            }

            ActionResult r;
            r.outputs["path"] = path->str();
            r.outputs["created"] = created ? "true" : "false";
            return r;
        }
    };

    std::unique_ptr<Action> makeMkdirAction() {
        return std::make_unique<MkdirAction>();
    }

} // namespace cajeta::buildtool
