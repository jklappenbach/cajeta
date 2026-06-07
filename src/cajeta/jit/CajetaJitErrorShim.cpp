//
// RTTI-free implementation of the llvm::Error consumers. See header for why
// this lives in its own TU (compiled -fno-rtti when the host LLVM has RTTI
// off — guarded in src/CMakeLists.txt).
//
#include "cajeta/jit/CajetaJitErrorShim.h"

namespace cajeta::jit {

    void consumeError(llvm::Error err) {
        llvm::consumeError(std::move(err));
    }

    std::string toString(llvm::Error err) {
        return llvm::toString(std::move(err));
    }

    void cantFail(llvm::Error err) {
        llvm::cantFail(std::move(err));
    }

} // namespace cajeta::jit
