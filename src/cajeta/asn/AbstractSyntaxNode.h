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
        // The file these tokens were parsed FROM — interned, so this costs a
        // pointer and only when --emit-xref is on. Null for a node built from a
        // synthesized re-parse (template instantiation, mock synthesis), whose
        // line numbers refer to a snippet rather than to any file. See
        // xref::internSourceFile.
        //
        // Not the same thing as "the module being compiled": a stdlib body is
        // generated while a user module is active.
        const string* sourceFile = nullptr;
        string sourceText;
        vector<AbstractSyntaxNodePtr> children;
    public:
        AbstractSyntaxNode(antlr4::Token* token) {
            if (token != nullptr) {
                sourceLine = token->getLine();
                sourceText = token->getText();
                sourceColumn = token->getCharPositionInLine();
                // Gated BEFORE getSourceName(), which returns a std::string by
                // value: without this a normal build would pay a string allocation
                // per AST node for an index it never asked for.
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

        // transform-intrinsics U7 — synthetic (parser-less) nodes carry the
        // span of the construct they desugar, so their diagnostics locate at
        // the source the user actually wrote.
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

        // "" when this node came from synthesized source (or xref capture is off).
        // An empty file is the signal to record NOTHING for this node: a position
        // with no file behind it cannot be navigated to, and guessing at one sends
        // the developer somewhere real and wrong.
        const string& getSourceFile() const {
            static const string kNone;
            return sourceFile ? *sourceFile : kNone;
        }

        const string& getSourceText() const {
            return sourceText;
        }

        vector<AbstractSyntaxNodePtr>& getChildren() { return children; }

        // Analysis-walk descent (title-tracking 7.2.4). `children` is the
        // CODEGEN child list; statements and calls keep their payloads in
        // private fields (if/loop/try bodies, return expressions, call and
        // ctor arguments), so a getChildren() walk misses whole subtrees
        // silently. Overrides visit those payloads AND the children — this
        // is the single descent primitive for analysis passes (retainsFormal,
        // computeLastUses, arenaWalk). Codegen must never use it: generating
        // "every sub-node" would emit call arguments twice.
        virtual void forEachSubNode(
                const std::function<void(const AbstractSyntaxNodePtr&)>& fn) {
            for (auto& child : children) {
                if (child) fn(child);
            }
        }

        // Pre-codegen pass: registers class/method signatures. See Compiler.cpp.
        virtual void generateSignature(CajetaModulePtr module) { }

        // Pre-codegen pass: resolves CajetaType information that codegen will need but
        // can't recover from LLVM types alone (fp8 vs i8, template instantiation,
        // etc.). Default walks children. Concrete Expression subclasses override to set
        // their own resolvedType once children have been resolved.
        virtual void resolveTypes(CajetaModulePtr module) {
            for (auto& child : children) {
                if (child) child->resolveTypes(module);
            }
        }

        virtual llvm::Value* generateCode(CajetaModulePtr module) = 0;
    };

    typedef shared_ptr<AbstractSyntaxNode> AbstractSyntaxNodePtr;
}
