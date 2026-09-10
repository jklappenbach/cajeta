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
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "ParserRuleContext.h"
#include "CajetaParser.h"
#include "cajeta/xref/XrefIndex.h"

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
        // Interned parse file, only under --emit-xref; null for synthesized source.
        const string* sourceFile = nullptr;
        string sourceText;
        vector<AbstractSyntaxNodePtr> children;
    public:
        AbstractSyntaxNode(antlr4::Token* token) {
            if (token != nullptr) {
                sourceLine = token->getLine();
                sourceText = token->getText();
                sourceColumn = token->getCharPositionInLine();
                // Gated BEFORE getSourceName(), which returns a std::string by value.
                if (xref::captureEnabled()) {
                    if (auto* stream = token->getInputStream()) {
                        sourceFile = xref::internSourceFile(stream->getSourceName());
                    }
                }
            } else {
                sourceLine = 0;
                sourceColumn = 0;
            }
        }

        void setSourceSpan(int line, int column) {
            sourceLine = line;
            sourceColumn = column;
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

        // "" for synthesized source, which means record NOTHING for this node.
        const string& getSourceFile() const {
            static const string kNone;
            return sourceFile ? *sourceFile : kNone;
        }

        const string& getSourceText() const {
            return sourceText;
        }

        vector<AbstractSyntaxNodePtr>& getChildren() { return children; }

        // The single descent primitive for analysis passes. `children` is only the
        // CODEGEN list, so overrides visit their private payloads too. Codegen must
        // never use it: emitting "every sub-node" would emit call arguments twice.
        virtual void forEachSubNode(
                const std::function<void(const AbstractSyntaxNodePtr&)>& fn) {
            for (auto& child : children) {
                if (child) fn(child);
            }
        }

        // Pre-codegen pass: registers class/method signatures. See Compiler.cpp.
        virtual void generateSignature(CajetaModulePtr module) { }

        // Pre-codegen pass: resolves the CajetaType information LLVM types cannot carry.
        virtual void resolveTypes(CajetaModulePtr module) {
            for (auto& child : children) {
                if (child) child->resolveTypes(module);
            }
        }

        virtual llvm::Value* generateCode(CajetaModulePtr module) = 0;
    };

    typedef shared_ptr<AbstractSyntaxNode> AbstractSyntaxNodePtr;
}
