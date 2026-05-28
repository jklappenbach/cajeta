//
// Why this file exists.
//
// ROCm / upstream LLVM is built with LLVM_ENABLE_RTTI=OFF, so it ships no
// C++ typeinfo for its own classes — it relies on LLVM's hand-rolled
// isa<>/dyn_cast<> RTTI (a per-class static `classID`) instead.
//
// llvm::consumeError / toString / cantFail expand inline (via
// handleAllErrors -> joinErrors) to code that instantiates the vtable and
// C++ typeinfo for llvm::ErrorInfo<ErrorList, ErrorInfoBase>. When that
// instantiation is compiled with the default -frtti, g++ emits that
// typeinfo and references `typeinfo for llvm::ErrorInfoBase`, which the
// -fno-rtti LLVM never emitted -> undefined reference at link.
//
// The natural home for these calls, JitTestHelper.cpp, transitively
// includes ANTLR headers that use typeid and therefore cannot itself be
// compiled -fno-rtti. So we isolate the llvm::Error consumers here, in a
// translation unit that includes only LLVM headers, and compile *just this
// file* with -fno-rtti (see test/CMakeLists.txt). Under -fno-rtti g++ emits
// no typeinfo at all, so nothing dangles. Against a distro LLVM built with
// RTTI on, the symbol exists and the -fno-rtti guard is a no-op.
//
#include "JitErrorShim.h"

namespace cajeta::jittest {

    void consumeError(llvm::Error err) {
        llvm::consumeError(std::move(err));
    }

    std::string toString(llvm::Error err) {
        return llvm::toString(std::move(err));
    }

    void cantFail(llvm::Error err) {
        llvm::cantFail(std::move(err));
    }

} // namespace cajeta::jittest
