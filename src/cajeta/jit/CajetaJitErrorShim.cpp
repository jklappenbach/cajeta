// RTTI-free implementation of the llvm::Error consumers, in its own TU so it can
// be compiled -fno-rtti when the host LLVM has RTTI off (see src/CMakeLists.txt).
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
