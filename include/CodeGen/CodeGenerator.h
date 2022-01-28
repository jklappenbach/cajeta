#ifndef TINYLANG_CODEGEN_CODEGENERATOR_H
#define TINYLANG_CODEGEN_CODEGENERATOR_H

#include "tinylang/AST/AST.h"
#include "tinylang/AST/ASTContext.h"
#include "llvm/Target/TargetMachine.h"
#include <string>

namespace cajeta {

    class CodeGenerator {
        llvm::LLVMContext& ctxLlvm;
        ASTContext& ctxAst;
        llvm::TargetMachine* targetMachine;
        ModuleDeclaration* moduleDeclaration;

    protected:
        CodeGenerator(llvm::LLVMContext& ctxLlvm, ASTContext& ctxAst, llvm::TargetMachine* targetMachine)
                : ctxLlvm(ctxLlvm), ctxAst(ctxAst), targetMachine(targetMachine), moduleDeclaration(nullptr) {}

    public:
        static CodeGenerator* create(llvm::LLVMContext& ctxLlvm, ASTContext& ctxAst, llvm::TargetMachine* targetMachine);

        std::unique_ptr<llvm::Module> run(ModuleDeclaration* moduleDeclaration, std::string fileName);
    };
} // namespace cajeta
#endif