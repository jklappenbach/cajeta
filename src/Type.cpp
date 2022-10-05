//
// Created by James Klappenbach on 10/2/22.
//

#include "cajeta/Type.h"
#include "cajeta/Field.h"

namespace cajeta {
    map<QualifiedName*, Type*> Type::global;

    //    CHAR:               'char';
    //    INT16:              'int16';
    //    UINT16:             'uint16';
    //    INT32:              'int32';
    //    UINT32:             'uint32';
    //    INT64:              'int64';
    //    UINT64:             'uint64';
    //    INT128:             'int128';
    //    UINT128:            'uint128';
    //    FINALLY:            'finally';
    //    FLOAT16:            'float16';
    //    FLOAT32:            'float32';
    //    FLOAT64:            'float64';
    llvm::Type* Type::getLlvmType(llvm::LLVMContext* context) {
        if (llvmType == nullptr) {
            // See if we're a primitive datatype
            if (qName->getPackageName().empty()) {
                if (qName->getTypeName() == "void") {
                    llvmType = llvm::Type::getVoidTy(*context);
                } else if (qName->getPackageName() == "char") {
                    llvmType = llvm::Type::getInt8Ty(*context);
                } else if (qName->getPackageName() == "int16") {
                    llvmType = llvm::Type::getInt16Ty(*context);
                } else if (qName->getPackageName() == "uint16") {
                    llvmType = llvm::Type::getInt16Ty(*context);
                } else if (qName->getPackageName() == "int32") {
                    llvmType = llvm::Type::getInt32Ty(*context);
                } else if (qName->getPackageName() == "uint32") {
                    llvmType = llvm::Type::getInt32Ty(*context);
                } else if (qName->getPackageName() == "int64") {
                    llvmType = llvm::Type::getInt64Ty(*context);
                } else if (qName->getPackageName() == "uint64") {
                    llvmType = llvm::Type::getInt64Ty(*context);
                } else if (qName->getPackageName() == "int128") {
                    llvmType = llvm::Type::getInt128Ty(*context);
                } else if (qName->getPackageName() == "uint128") {
                    llvmType = llvm::Type::getInt128Ty(*context);
                } else if (qName->getPackageName() == "float16") {
                    llvmType = llvm::Type::getBFloatTy(*context);
                } else if (qName->getPackageName() == "float32") {
                    llvmType = llvm::Type::getFloatTy(*context);
                } else if (qName->getPackageName() == "float64") {
                    llvmType = llvm::Type::getDoubleTy(*context);
                } else if (qName->getPackageName() == "float128") {
                    llvmType = llvm::Type::getFP128Ty(*context);
                }
            }
        }
        return llvmType;
    }

    cajeta::Type* cajeta::Type::fromContext(CajetaParser::TypeTypeContext* ctxType) {
        bool reference = false;
        Type* type = nullptr;
        QualifiedName* qName;
        if (ctxType != nullptr) {
            CajetaParser::PrimitiveTypeContext* ctxPrimitiveType = ctxType->primitiveType();
            if (ctxPrimitiveType != nullptr) {
                qName = QualifiedName::toQualifiedName(ctxPrimitiveType->getText(), "cajeta");
            } else {
                CajetaParser::ClassOrInterfaceTypeContext* ctxClassOrInterface = ctxType->classOrInterfaceType();
                if (ctxClassOrInterface != nullptr) {
                    qName = QualifiedName::toQualifiedName(ctxClassOrInterface);
                }
            }
            type = global[qName];
        }
        return type;
    }
}