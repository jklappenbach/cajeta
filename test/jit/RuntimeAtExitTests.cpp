// The runtime bitcode links into every JIT session, where an `atexit`
// reference is rewritten to LLJIT's platform helper and a session without that
// platform cannot initialize. Exit work goes through __cajeta_prof_shutdown.

#include <gtest/gtest.h>

#include "cajeta/runtime/EmbeddedRuntime.h"

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/MemoryBuffer.h"

TEST(RuntimeAtExitTests, theEmbeddedRuntimeReferencesNoAtExit) {
    llvm::LLVMContext ctx;
    llvm::StringRef bc(reinterpret_cast<const char*>(cajeta_runtime_bc), cajeta_runtime_bc_len);
    auto buf = llvm::MemoryBuffer::getMemBuffer(bc, "cajeta_runtime", false);
    auto parsed = llvm::parseBitcodeFile(buf->getMemBufferRef(), ctx);
    ASSERT_TRUE((bool) parsed) << llvm::toString(parsed.takeError());
    for (const char* name : {"atexit", "__cxa_atexit", "on_exit"}) {
        llvm::Function* f = (*parsed)->getFunction(name);
        EXPECT_TRUE(f == nullptr || f->use_empty())
            << "the runtime calls " << name << ", which a JIT session cannot run";
    }
}
