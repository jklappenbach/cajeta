//
// Created by James Klappenbach on 10/22/22.
//

#pragma once

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "ParserRuleContext.h"
#include "CajetaParser.h"

using namespace std;

namespace cajeta {
    class CajetaModule;
    typedef shared_ptr<CajetaModule> CajetaModulePtr;

    class AbstractSyntaxNode;
    typedef shared_ptr<AbstractSyntaxNode> AbstractSyntaxNodePtr;

    class AbstractSyntaxNode : public std::enable_shared_from_this<AbstractSyntaxNode> {
    protected:
        int sourceLine;
        int sourceColumn;
        string sourceText;
        vector<AbstractSyntaxNodePtr> children;
    public:
        AbstractSyntaxNode(antlr4::Token* token) {
            if (token != nullptr) {
                sourceLine = token->getLine();
                sourceText = token->getText();
                sourceColumn = token->getCharPositionInLine();
            }
        }

        void addChild(AbstractSyntaxNodePtr child) {
            children.push_back(child);
        }

        int getSourceLine() const {
            return sourceLine;
        }

        int getSourceColumn() const {
            return sourceColumn;
        }

        const string& getSourceText() const {
            return sourceText;
        }

        vector<AbstractSyntaxNodePtr>& getChildren() { return children; }

        virtual void generateSignature(CajetaModulePtr module) { }

        virtual llvm::Value* generateCode(CajetaModulePtr module) = 0;
    };

    typedef shared_ptr<AbstractSyntaxNode> AbstractSyntaxNodePtr;
}
