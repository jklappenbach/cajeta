#include "BuildErrors.h"

#include <iostream>

#include "cajeta/error/Diagnostics.h"

namespace cajeta::buildtool {

    char ManifestError::ID = 0;
    char BuildStepError::ID = 0;

    void ManifestError::log(llvm::raw_ostream& os) const {
        if (!file.empty()) os << file;
        if (!file.empty() && !keyPath.empty()) os << ".";
        os << keyPath << ": " << message;
    }

    std::error_code ManifestError::convertToErrorCode() const {
        return llvm::inconvertibleErrorCode();
    }

    void BuildStepError::log(llvm::raw_ostream& os) const { os << step << ": " << message; }

    std::error_code BuildStepError::convertToErrorCode() const {
        return llvm::inconvertibleErrorCode();
    }

    void reportTaskError(const std::string& taskName, llvm::Error err) {
        if (!jsonProgressEnabled()) {
            std::string msg;
            llvm::raw_string_ostream os(msg);
            os << std::move(err);
            std::cerr << "cajeta " << taskName << ": " << os.str() << "\n";
            return;
        }
        llvm::handleAllErrors(
            std::move(err),
            [](const ManifestError& e) {
                emitJsonDiagnostic("error", "CAJETA_ERROR_MANIFEST", e.message, e.file, -1, -1,
                                   "project", GeneratedOrigin(),
                                   DiagnosticArtifact{"manifest",
                                       e.keyPath.empty() ? "(root)" : e.keyPath, ""});
            },
            [](const BuildStepError& e) {
                emitJsonDiagnostic("error", "CAJETA_ERROR_BUILD_STEP", e.message, "", -1, -1,
                                   "project", GeneratedOrigin(),
                                   DiagnosticArtifact{"build-step", e.step, ""});
            },
            [&](const llvm::ErrorInfoBase& e) {
                emitJsonDiagnostic("error", "", "cajeta " + taskName + ": " + e.message());
            });
    }

} // namespace cajeta::buildtool
