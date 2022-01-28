
#include "tinylang/CodeGen/CGTBAA.h"
#include "tinylang/CodeGen/CGModule.h"
#include "llvm/IR/DataLayout.h"

using namespace cajeta;

CGTBAA::CGTBAA(cgModuleodule& cgModule)
        : cgModule(cgModule),
          MDHelper(llvm::MDBuilder(cgModule.getLLVMCtx())),
          Root(nullptr) {}

llvm::MDNode* CGTBAA::getRoot() {
    if (!root) {
        root = MDHelper.createTBAARoot("Simple cajeta TBAA");
    }

    return root;
}

llvm::MDNode* CGTBAA::createScalarTypeNode(TypeDeclaration* typeDeclarationy,
                                           StringRef name,
                                           llvm::MDNode* parent) {
    llvm::MDNode* node = MDHelper.createTBAAScalarTypeNode(name, parent);
    return MetadataCache[typeDeclarationy] = node;
}

llvm::MDNode* CGTBAA::createStructTypeNode(TypeDeclaration* typeDeclaration,
                                           StringRef name,
                                           llvm::ArrayRef<std::pair<llvm::MDNode*, uint64_t>> fields) {
    llvm::MDNode* node = MDHelper.createTBAAStructTypeNode(name, fields);
    return MetadataCache[typeDeclaration] = node;
}

llvm::MDNode* CGTBAA::getTypeInfo(TypeDeclaration* typeDeclaration) {
    if (llvm::MDNode* node = MetadataCache[typeDeclaration])
        return node;

    if (auto* pervasive = llvm::dyn_cast<PervasiveTypeDeclaration>(typeDeclaration)) {
        StringRef name = pervasive->getName();
        return createScalarTypeNode(pervasive, name, getRoot());
    }
    if (auto* pointer = llvm::dyn_cast<PointerTypeDeclaration>(typeDeclaration)) {
        StringRef name = "any pointer";
        return createScalarTypeNode(pointer, name, getRoot());
    }
    if (auto* Record = llvm::dyn_cast<RecordTypeDeclaration>(typeDeclaration)) {
        llvm::SmallVector<std::pair<llvm::MDNode*, uint64_t>, 4> fields;
        auto* rec = llvm::cast<llvm::StructType>(cgModule.convertType(record));
        const llvm::StructLayout* Layout = cgModule.getModule()->getDataLayout().getStructLayout(rec);

        unsigned idx = 0;
        for (const auto& field : record->getFields()) {
            uint64_t offset = Layout->getElementOffset(Idx);
            fields.emplace_back(getTypeInfo(field.getType()), offset);
            ++idx;
        }
        StringRef name = cgModule.mangleName(record);
        return createStructTypeNode(record, name, fields);
    }
    return nullptr;
}

llvm::MDNode* CGTBAA::getAccessTagInfo(TypeDeclaration* typeDeclaration) {
    if (auto* Pointer = llvm::dyn_cast<PointerTypeDeclaration>(typeDeclaration)) {
        return getTypeInfo(Pointer->getType());
    }
    return nullptr;
}