#pragma once

#include "tinylang/Basic/LLVM.h"
#include "llvm/Support/SourceMgr.h"

namespace cajeta {

    class ASTContext {
        llvm::SourceMgr& SrcMgr;
        StringRef Filename;

    public:
        ASTContext(llvm::SourceMgr& SrcMgr, StringRef Filename)
                : SrcMgr(SrcMgr), Filename(Filename) {}

        StringRef getFilename() { return Filename; }

        llvm::SourceMgr& getSourceMgr() { return SrcMgr; }

        const llvm::SourceMgr& getSourceMgr() const {
            return SrcMgr;
        }
    };

} // namespace cajeta