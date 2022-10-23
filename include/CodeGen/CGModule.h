#ifndef TINYLANG_CODEGEN_CGMODULE_H
#define TINYLANG_CODEGEN_CGMODULE_H

#include "AST/AST.h"
#include "AST/ASTContext.h"
#include "CodeGen/CGDebugInfo.h"
#include "CodeGen/CGTBAA.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace cajeta {

    class CGModule {
        ASTContext& astContext;
        llvm::Module* module;

        ModuleDeclaration* moduleDeclaration;

        llvm::DenseMap<TypeDeclaration*, llvm::Type*> typeCache;

        // Repository of cache objects.
        llvm::DenseMap<Declaration*, llvm::GlobalObject*> globals;

        CGTBAA TBAA;
        std::unique_ptr<CGDebugInfo> debugInfo;

    public:
        llvm::Type* voidType;
        llvm::Type* int1Type;
        llvm::Type* int32Type;
        llvm::Type* int64Type;
        llvm::Constant* int32Zero;

    public:
        CGModule(ASTContext& astContext, llvm::Module* module);

        void initialize();

        ASTContext& getASTCtx() { return astContext; }

        llvm::LLVMContext& getLLVMCtx() {
            return module->getContext();
        }

        llvm::Module* getModule() { return module; }

        ModuleDeclaration* getModuleDeclaration() { return moduleDeclaration; }

        void decorateInst(llvm::Instruction* instruction,
                          TypeDeclaration* type);

        llvm::Type* convertType(TypeDeclaration* typeDeclaration);

        std::string mangleName(Declaration* decl);

        llvm::GlobalObject* getGlobal(Declaration* decl);

        CGDebugInfo* getDbgInfo() {
            return debugInfo.get();
        }

        void applyLocation(llvm::Instruction* instruction, llvm::SMLoc smLoc);

        void run(ModuleDeclaration* moduleDeclaration);
    };
} // namespace cajeta
#endif