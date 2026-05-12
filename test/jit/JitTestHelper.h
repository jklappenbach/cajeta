//
// JIT helper for Cajeta integration tests. Compiles a Cajeta source string,
// links the embedded runtime, and JITs the resulting module so tests can call
// generated functions directly and assert against the returned value.
//

#pragma once

#include <memory>
#include <string>

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"

namespace cajeta {
    class Compiler;
    class CajetaModule;
}

namespace cajeta_test {

class CajetaJit {
public:
    struct Options {
        bool boundsCheckEnabled = true;
    };

    // Compile `source` (a Cajeta compilation unit) into a JIT instance. The class
    // must match the file path mapping the compiler expects (we infer the package
    // from `fqClassName`). The two overloads exist because default-aggregate-init
    // for a separately-declared struct trips older C++17 dialects.
    static std::unique_ptr<CajetaJit> compile(const std::string& source,
                                              const std::string& fqClassName);
    static std::unique_ptr<CajetaJit> compile(const std::string& source,
                                              const std::string& fqClassName,
                                              const Options& opts);

    ~CajetaJit();

    // Look up a method by short name (e.g. "add"). Returns a callable pointer cast
    // to T, or nullptr if not found.
    template <typename T>
    T lookup(const std::string& shortName) {
        return reinterpret_cast<T>(lookupAddress(shortName));
    }

private:
    CajetaJit();

    void* lookupAddress(const std::string& shortName);

    std::unique_ptr<llvm::orc::LLJIT> jit;
    // Mapping short method name -> full mangled name in the JIT'd module. Built
    // once at compile time so per-test lookups don't have to rescan.
    std::map<std::string, std::string> nameMap;
};

} // namespace cajeta_test
