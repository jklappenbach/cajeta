//
// Created by James Klappenbach on 10/2/22.
//

#include "cajeta/type/CajetaType.h"
#include "cajeta/type/Field.h"

namespace cajeta {
    map<QualifiedName*, CajetaType*> CajetaType::global;

#define CAJETA_NATIVE_PACKAGE "cajeta"
    void CajetaType::init(llvm::LLVMContext* ctxLlvm) {
        QualifiedName* qName = QualifiedName::create("void", CAJETA_NATIVE_PACKAGE);
        //CajetaType::global[qName] = llvm::Type::getVoidTy(*ctxLlvm);
    }

    llvm::Type* CajetaType::getLlvmType(llvm::LLVMContext* ctxLlvm) {
        if (llvmType == nullptr) {
            // See if we're a primitive datatype
            if (qName->getPackageName() == "cajeta") {
                if (qName->getTypeName() == "void") {
                    llvmType = llvm::Type::getVoidTy(*ctxLlvm);
                } else if (qName->getTypeName() == "char") {
                    llvmType = llvm::Type::getInt8Ty(*ctxLlvm);
                } else if (qName->getTypeName() == "int16") {
                    llvmType = llvm::Type::getInt16Ty(*ctxLlvm);
                } else if (qName->getTypeName() == "uint16") {
                    llvmType = llvm::Type::getInt16Ty(*ctxLlvm);
                } else if (qName->getTypeName() == "int32") {
                    llvmType = llvm::Type::getInt32Ty(*ctxLlvm);
                } else if (qName->getTypeName() == "uint32") {
                    llvmType = llvm::Type::getInt32Ty(*ctxLlvm);
                } else if (qName->getTypeName() == "int64") {
                    llvmType = llvm::Type::getInt64Ty(*ctxLlvm);
                } else if (qName->getTypeName() == "uint64") {
                    llvmType = llvm::Type::getInt64Ty(*ctxLlvm);
                } else if (qName->getTypeName() == "int128") {
                    llvmType = llvm::Type::getInt128Ty(*ctxLlvm);
                } else if (qName->getTypeName() == "uint128") {
                    llvmType = llvm::Type::getInt128Ty(*ctxLlvm);
                } else if (qName->getTypeName() == "float16") {
                    llvmType = llvm::Type::getBFloatTy(*ctxLlvm);
                } else if (qName->getTypeName() == "float32") {
                    llvmType = llvm::Type::getFloatTy(*ctxLlvm);
                } else if (qName->getTypeName() == "float64") {
                    llvmType = llvm::Type::getDoubleTy(*ctxLlvm);
                } else if (qName->getTypeName() == "float128") {
                    llvmType = llvm::Type::getFP128Ty(*ctxLlvm);
                }
            } else {
                vector<llvm::Type*> llvmMembers;
                for (Field* field : this->fields) {
                    llvmMembers.push_back(field->getType()->getLlvmType(ctxLlvm));
                }

                this->llvmType = llvm::StructType::create(*ctxLlvm, llvm::ArrayRef<llvm::Type*>(llvmMembers), llvm::StringRef(qName->getTypeName()));
            }
        }
        return llvmType;
    }

    cajeta::CajetaType* cajeta::CajetaType::fromContext(CajetaParser::TypeTypeContext* ctxType) {
        bool reference = false;
        CajetaType* type = nullptr;
        QualifiedName* qName;
        if (ctxType != nullptr) {
            CajetaParser::PrimitiveTypeContext* ctxPrimitiveType = ctxType->primitiveType();
            if (ctxPrimitiveType != nullptr) {
                qName = QualifiedName::create(ctxPrimitiveType->getText(), "cajeta");
            } else {
                CajetaParser::ClassOrInterfaceTypeContext* ctxClassOrInterface = ctxType->classOrInterfaceType();
                if (ctxClassOrInterface != nullptr) {
                    qName = QualifiedName::fromContext(ctxClassOrInterface);
                }
            }
            type = global[qName];
        }
        return type;
    }

    void CajetaType::addField(Field* field) {
        this->fields.push_back(field);
    }
    void CajetaType::addFields(list<Field*> fields) {
        this->fields.insert(this->fields.end(), fields.begin(), fields.end());
    }
}