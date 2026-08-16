#include "MigrationSwitch.h"

#include "cajeta/error/DiagnosticEngine.h"

#include "llvm/Support/raw_ostream.h"

#include <cstdlib>

namespace cajeta::ownership {

    bool MigrationSwitch::warns() const {
        if (override_ >= 0) return override_ == 1;
        const char* mode = std::getenv(envVar_);
        return mode && std::string(mode) == "warn";
    }

    void MigrationSwitch::report(const std::string& note,
                                 const std::string& warnCode,
                                 const std::string& message,
                                 const std::string& file, int line) const {
        llvm::errs() << "cajeta: note: " << note << "\n";
        if (DiagnosticEngine* eng = DiagnosticEngine::active()) {
            eng->report("warning", warnCode, message, file, line, -1);
        }
    }

}  // namespace cajeta::ownership
