#include "CodegenGuard.h"

#include "llvm/Support/ErrorHandling.h"

namespace cajeta {
namespace xpu {

namespace {

struct CodegenFatal {
    std::string reason;
};

void throwCodegenFatal(void* /*userData*/, const char* reason, bool /*genCrashDiag*/) {
    throw CodegenFatal{reason != nullptr ? reason : "LLVM fatal error"};
}

}  // namespace

bool runGuardedCodegen(const std::function<void()>& emit, std::string& reason) {
    llvm::ScopedFatalErrorHandler guard(throwCodegenFatal, nullptr);
    try {
        emit();
    } catch (const CodegenFatal& fatal) {
        reason = fatal.reason;
        return false;
    }
    return true;
}

}  // namespace xpu
}  // namespace cajeta
