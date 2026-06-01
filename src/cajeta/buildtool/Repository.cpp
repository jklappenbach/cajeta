#include "cajeta/buildtool/Repository.h"

#include "cajeta/buildtool/repo/FilesystemRepository.h"
#include "cajeta/buildtool/repo/HttpRepository.h"

#include <llvm/Support/Error.h>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

    } // namespace

    llvm::Expected<std::vector<RepositoryPtr>> buildRepositories(
        const std::vector<RepositorySpec>& specs,
        const std::string& downloadStageDir) {
        std::vector<RepositoryPtr> out;
        out.reserve(specs.size());
        for (const auto& spec : specs) {
            if (spec.type == "filesystem") {
                out.push_back(std::make_shared<FilesystemRepository>(
                    spec.name, spec.path));
            } else if (spec.type == "http") {
                if (downloadStageDir.empty()) {
                    return err("repository '" + spec.name +
                               "': HTTP driver requires a download-stage "
                               "directory (caller must pass one — "
                               "see buildRepositories docs)");
                }
                out.push_back(std::make_shared<HttpRepository>(
                    spec.name, spec.url, spec.auth, downloadStageDir));
            } else if (spec.type == "maven-compat") {
                return err("repository '" + spec.name +
                           "' uses type='maven-compat' — shim "
                           "lands in Phase 6b (follow-up to HTTP).");
            } else if (spec.type == "git") {
                return err("repository '" + spec.name +
                           "' uses type='git' — Git driver lands "
                           "in Phase 6c.");
            } else {
                return err("repository '" + spec.name +
                           "': unknown type '" + spec.type + "'");
            }
        }
        return out;
    }

} // namespace cajeta::buildtool
