#include "CompilationContext.h"

namespace cajeta {

namespace {
thread_local CompilationContext* g_currentCtx = nullptr;
}

CompilationContext* currentCompilationContext() { return g_currentCtx; }

void setCurrentCompilationContext(CompilationContext* ctx) { g_currentCtx = ctx; }

llvm::LLVMContext* currentLlvmContext() {
    return g_currentCtx ? g_currentCtx->llvmContext : nullptr;
}

} // namespace cajeta
