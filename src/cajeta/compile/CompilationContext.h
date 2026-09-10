#pragma once

namespace llvm { class LLVMContext; }

namespace cajeta {

// Owns one compilation's mutable, per-thread state so independent compiles can run concurrently in one process.
class CompilationContext {
public:
    CompilationContext() = default;
    CompilationContext(const CompilationContext&) = delete;
    CompilationContext& operator=(const CompilationContext&) = delete;

    // The LLVMContext this compile builds into; null until the Compiler installs it.
    llvm::LLVMContext* llvmContext = nullptr;
};

// The calling thread's current compilation context (nullptr outside a compile).
CompilationContext* currentCompilationContext();
void setCurrentCompilationContext(CompilationContext* ctx);

// The calling thread's active LLVMContext, or nullptr when none is installed yet.
llvm::LLVMContext* currentLlvmContext();

// RAII: makes `ctx` current for the enclosing scope and restores the previous on exit.
struct ScopedCompilationContext {
    explicit ScopedCompilationContext(CompilationContext* ctx)
        : prev(currentCompilationContext()) { setCurrentCompilationContext(ctx); }
    ~ScopedCompilationContext() { setCurrentCompilationContext(prev); }
    ScopedCompilationContext(const ScopedCompilationContext&) = delete;
    ScopedCompilationContext& operator=(const ScopedCompilationContext&) = delete;

private:
    CompilationContext* prev;
};

} // namespace cajeta
