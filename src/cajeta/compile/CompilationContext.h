#pragma once

namespace llvm { class LLVMContext; }

namespace cajeta {

// Owns one compilation's mutable, per-thread state so independent compiles can
// run concurrently on threads in one process (see
// docs/specs/compiler-threadsafe-spec.md). Unit 1 establishes the thread-local
// seam only; the ~49 process-global registries migrate onto it in later units.
class CompilationContext {
public:
    CompilationContext() = default;
    CompilationContext(const CompilationContext&) = delete;
    CompilationContext& operator=(const CompilationContext&) = delete;

    // U6.4.2 — the LLVMContext this compile builds into (the owning Compiler's
    // per-thread context). The frozen-stdlib rebuild path reads this (via
    // currentLlvmContext()) to materialize a shared semantic type's binding in
    // THIS thread's context. Null until the Compiler installs it.
    llvm::LLVMContext* llvmContext = nullptr;
};

// The calling thread's current compilation context (nullptr outside a compile).
// Backed by a thread_local pointer, so each thread resolves to its own context.
CompilationContext* currentCompilationContext();
void setCurrentCompilationContext(CompilationContext* ctx);

// U6.4.2 — the calling thread's active LLVMContext (its current CompilationContext's
// llvmContext), or nullptr if there's no context or it hasn't been installed yet.
llvm::LLVMContext* currentLlvmContext();

// RAII: make `ctx` current for the enclosing scope and restore the previous
// current on exit (nesting-safe: a prime context stays current beneath a
// per-test context).
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
