//
// Created by James Klappenbach on 10/5/22.
//

#pragma once

#include "llvm/IR/BasicBlock.h"
#include "Block.h"

#include "list"

using namespace std;

namespace cajeta {
    class Method;

    class BlockStatement;

    class DefaultBlock : public Block {
    public:
        DefaultBlock() { }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };
}

