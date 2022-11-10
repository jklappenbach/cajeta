//
// Created by James Klappenbach on 10/5/22.
//

#pragma once

#include <llvm/IR/BasicBlock.h>
#include <cajeta/asn/BlockStatement.h>

#include <list>

using namespace std;

namespace cajeta {
    class Method;
    class BlockStatement;

    class DefaultConstructorBlock : public Block {
    public:
        DefaultConstructorBlock() { }
        llvm::Value* generateCode(CajetaModule* module) override;
    };
}

