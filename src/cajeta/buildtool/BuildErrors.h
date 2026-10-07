// Build-tool errors that name a non-source subject (specs/diagnostic-location-spec.md 4).

#pragma once

#include <string>

#include <llvm/Support/Error.h>

namespace cajeta::buildtool {

    // An error about one manifest key: printed as `<file>.<keyPath>: <message>`.
    class ManifestError : public llvm::ErrorInfo<ManifestError> {
    public:
        static char ID;
        ManifestError(std::string file, std::string keyPath, std::string message)
            : file(std::move(file)), keyPath(std::move(keyPath)), message(std::move(message)) {}
        void log(llvm::raw_ostream& os) const override;
        std::error_code convertToErrorCode() const override;

        std::string file;
        std::string keyPath;
        std::string message;
    };

    // A build step that failed with no source position: printed as `<step>: <message>`.
    class BuildStepError : public llvm::ErrorInfo<BuildStepError> {
    public:
        static char ID;
        BuildStepError(std::string step, std::string message)
            : step(std::move(step)), message(std::move(message)) {}
        void log(llvm::raw_ostream& os) const override;
        std::error_code convertToErrorCode() const override;

        std::string step;
        std::string message;
    };

    // Reports `cajeta <task>`'s failure: the text line as before, or under --diag-format=json a
    // diagnostic whose artifact names the manifest key or the build step.
    void reportTaskError(const std::string& taskName, llvm::Error err);

} // namespace cajeta::buildtool
