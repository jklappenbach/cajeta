//
// Created by James Klappenbach on 10/2/22.
//

#include "cajeta/type/CajetaType.h"
#include "cajeta/type/Field.h"
#include "cajeta/compile/CajetaModule.h"

#define NATIVE_TYPE_ENTRY(typeName, llvmType) new CajetaType(QualifiedName::getOrInsert(typeName, CAJETA_NATIVE_PACKAGE), llvmType)
#define CAJETA_NATIVE_PACKAGE "cajeta"

namespace cajeta {
    map<string, CajetaType*> CajetaType::archive;

    void CajetaType::init(llvm::LLVMContext& ctx) {
        NATIVE_TYPE_ENTRY("void", llvm::Type::getVoidTy(ctx));
        NATIVE_TYPE_ENTRY("char", llvm::Type::getInt8Ty(ctx));
        NATIVE_TYPE_ENTRY("int16", llvm::Type::getInt16Ty(ctx));
        NATIVE_TYPE_ENTRY("uint16", llvm::Type::getInt16Ty(ctx));
        NATIVE_TYPE_ENTRY("int32", llvm::Type::getInt32Ty(ctx));
        NATIVE_TYPE_ENTRY("uint32", llvm::Type::getInt32Ty(ctx));
        NATIVE_TYPE_ENTRY("int64", llvm::Type::getInt64Ty(ctx));
        NATIVE_TYPE_ENTRY("uint64", llvm::Type::getInt64Ty(ctx));
        NATIVE_TYPE_ENTRY("int128", llvm::Type::getInt128Ty(ctx));
        NATIVE_TYPE_ENTRY("uint128", llvm::Type::getInt128Ty(ctx));
        NATIVE_TYPE_ENTRY("float16", llvm::Type::getBFloatTy(ctx));
        NATIVE_TYPE_ENTRY("float32", llvm::Type::getFloatTy(ctx));
        NATIVE_TYPE_ENTRY("float64", llvm::Type::getDoubleTy(ctx));
        NATIVE_TYPE_ENTRY("float128", llvm::Type::getFP128Ty(ctx));
    }

    map<string, CajetaType*>& CajetaType::getArchive() { return archive; }

    CajetaType* CajetaType::of(string typeName) {
        QualifiedName* qName = QualifiedName::getOrInsert(typeName, "cajeta");
        return CajetaType::archive[qName->toCanonical()];
    }

    CajetaType* CajetaType::of(string typeName, string package) {
        QualifiedName* qName = QualifiedName::getOrInsert(typeName, package);
        return CajetaType::archive[qName->toCanonical()];
    }

    CajetaType* CajetaType::fromContext(CajetaParser::PrimitiveTypeContext* ctx) {
        QualifiedName* qName = QualifiedName::getOrInsert(ctx->getText(), "cajeta");
        return CajetaType::archive[qName->toCanonical()];
    }

    cajeta::CajetaType* cajeta::CajetaType::fromContext(CajetaParser::TypeTypeOrVoidContext* ctx) {
        CajetaType* type = nullptr;
        if (ctx != nullptr) {
            if (ctx->VOID() != nullptr) {
                QualifiedName* qName = QualifiedName::getOrInsert(ctx->getText(), "cajeta");
                type = CajetaType::archive[qName->toCanonical()];
            } else {
                type = fromContext(ctx->typeType());
            }
        }
        return type;
    }

    cajeta::CajetaType* cajeta::CajetaType::fromContext(CajetaParser::TypeTypeContext* ctxType) {
        CajetaType* type = nullptr;
        if (ctxType != nullptr) {
            QualifiedName* qName;
            CajetaParser::PrimitiveTypeContext* ctxPrimitiveType = ctxType->primitiveType();
            if (ctxPrimitiveType != nullptr) {
                qName = QualifiedName::getOrInsert(ctxPrimitiveType->getText(), CAJETA_NATIVE_PACKAGE);
                type = archive[qName->toCanonical()];
            } else {
                CajetaParser::ClassOrInterfaceTypeContext* ctxClassOrInterface = ctxType->classOrInterfaceType();
                if (ctxClassOrInterface != nullptr) {
                    qName = QualifiedName::fromContext(ctxClassOrInterface);
                }
                type = archive[qName->toCanonical()];
            }
        }
        return type;
    }
}