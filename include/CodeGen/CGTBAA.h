#ifndef TINYLANG_CODEGEN_CGTBAA_H
#define TINYLANG_CODEGEN_CGTBAA_H

#include "AST/AST.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Metadata.h"

namespace cajeta {

    class CGModule;

    class CGTBAA {
        CGModule& cgModule;

        // MDHelper - Helper for creating metadata.
        llvm::MDBuilder mdBuilder;

        // The root node of the TBAA hierarchy
        llvm::MDNode* root;

        llvm::DenseMap<TypeDeclaration*, llvm::MDNode*> metadataCache;

        llvm::MDNode* createScalarTypeNode(TypeDeclaration* typeDeclaration,
                                           StringRef name,
                                           llvm::MDNode* parent);

        llvm::MDNode* createStructTypeNode(
                TypeDeclaration* typeDeclaration, StringRef name,
                llvm::ArrayRef<std::pair<llvm::MDNode*, uint64_t>>
                fields);

    public:
        CGTBAA(CGModule& cgModule);

        llvm::MDNode* getRoot();

        llvm::MDNode* getTypeInfo(TypeDeclaration* typeDeclaration);

        llvm::MDNode* getAccessTagInfo(TypeDeclaration* typeDeclaration);
    };
} // namespace cajeta
#endif