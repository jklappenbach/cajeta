//
// Created by James Klappenbach on 10/2/22.
//

#include "CajetaType.h"
#include "../field/Field.h"
#include "../compile/CajetaModule.h"
#include "CajetaArray.h"
#include "../error/InvalidOperandException.h"

namespace cajeta {

    #define CAJETA_NATIVE_PACKAGE ""
    #define NATIVE_TYPE_ENTRY(typeName, llvmType, typeFlags) CajetaType::create(QualifiedName::getOrInsert(typeName, CAJETA_NATIVE_PACKAGE), llvmType, typeFlags);

    map<string, CajetaTypePtr> CajetaType::canonicalMap;
    map<TypeKey, CajetaTypePtr> CajetaType::typeMap;
    map<llvm::Type::TypeID, CajetaTypePtr> CajetaType::llvmTypeIdMap;


    TypeKey::TypeKey(llvm::Type* type) {
        typeId = type->getTypeID();
        switch (type->getTypeID()) {
            case llvm::Type::IntegerTyID:
                typeCode = type->getIntegerBitWidth();
                break;
            default:
                typeCode = 0;
                break;
        }
    }

    bool operator<(const TypeKey& a, const TypeKey& b) {
        if (a.typeId < b.typeId) {
            return true;
        }
        if (a.typeCode < b.typeCode) {
            return true;
        }
        return false;
    }

    void CajetaType::init(llvm::LLVMContext& ctx) {
        NATIVE_TYPE_ENTRY("void", llvm::Type::getVoidTy(ctx), VOID_TYPE_ID);
        NATIVE_TYPE_ENTRY("boolean", llvm::Type::getInt1Ty(ctx), BOOLEAN_TYPE_ID);
        NATIVE_TYPE_ENTRY("uchar", llvm::Type::getInt8Ty(ctx), UINT8_TYPE_ID);
        NATIVE_TYPE_ENTRY("char", llvm::Type::getInt8Ty(ctx), INT8_TYPE_ID);
        NATIVE_TYPE_ENTRY("uint16", llvm::Type::getInt16Ty(ctx), UINT16_TYPE_ID);
        NATIVE_TYPE_ENTRY("int16", llvm::Type::getInt16Ty(ctx), INT16_TYPE_ID);
        NATIVE_TYPE_ENTRY("uint32", llvm::Type::getInt32Ty(ctx), UINT32_TYPE_ID);
        NATIVE_TYPE_ENTRY("int32", llvm::Type::getInt32Ty(ctx), INT32_TYPE_ID);
        NATIVE_TYPE_ENTRY("uint64", llvm::Type::getInt64Ty(ctx), UINT64_TYPE_ID);
        NATIVE_TYPE_ENTRY("int64", llvm::Type::getInt64Ty(ctx), INT64_TYPE_ID);
        NATIVE_TYPE_ENTRY("uint128", llvm::Type::getInt128Ty(ctx), UINT128_TYPE_ID);
        NATIVE_TYPE_ENTRY("int128", llvm::Type::getInt128Ty(ctx), INT128_TYPE_ID);
        NATIVE_TYPE_ENTRY("float16", llvm::Type::getBFloatTy(ctx), FLOAT16_TYPE_ID);
        NATIVE_TYPE_ENTRY("float32", llvm::Type::getFloatTy(ctx), FLOAT32_TYPE_ID);
        NATIVE_TYPE_ENTRY("float64", llvm::Type::getDoubleTy(ctx), FLOAT64_TYPE_ID);
        NATIVE_TYPE_ENTRY("float128", llvm::Type::getFP128Ty(ctx), FLOAT128_TYPE_ID);
        NATIVE_TYPE_ENTRY("pointer", llvm::Type::getInt64PtrTy(ctx), POINTER_TYPE_ID);
    }

    llvm::ConstantInt* CajetaType::getTypeAllocSize(CajetaModulePtr module) {
        const llvm::DataLayout& dataLayout = module->getLlvmModule()->getDataLayout();
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*module->getLlvmContext()),
            dataLayout.getTypeAllocSize(llvmType));
    }

    string CajetaType::toGeneric() {
        if (typeFlags & PRIMITIVE_FLAG) {
            switch (llvmType->getTypeID()) {
                case llvm::Type::BFloatTyID:
                case llvm::Type::FloatTyID:
                case llvm::Type::DoubleTyID:
                case llvm::Type::X86_FP80TyID:
                case llvm::Type::FP128TyID:
                case llvm::Type::PPC_FP128TyID:
                case llvm::Type::IntegerTyID:
                    return "number";
                case llvm::Type::VoidTyID:
                    return "void";
                case llvm::Type::FunctionTyID:
                    return "function";
                case llvm::Type::PointerTyID:
                    return "pointer";
                default:
                    return "unknown";
            }
        } else {
            return canonical;
        }
    }

    map<string, CajetaTypePtr>& CajetaType::getCanonicalMap() { return canonicalMap; }

    CajetaTypePtr CajetaType::of(string typeName) {
        QualifiedNamePtr qName = QualifiedName::getOrCreate(typeName);
        return CajetaType::canonicalMap[qName->toCanonical()];
    }

    CajetaTypePtr CajetaType::of(string typeName, string package) {
        QualifiedNamePtr qName = QualifiedName::getOrInsert(typeName, package);
        return CajetaType::canonicalMap[qName->toCanonical()];
    }

    CajetaTypePtr CajetaType::of(QualifiedNamePtr qName) {
        return CajetaType::canonicalMap[qName->toCanonical()];
    }

    CajetaTypePtr CajetaType::fromContext(CajetaParser::PrimitiveTypeContext* ctx, CajetaModulePtr module) {
        QualifiedNamePtr qName = QualifiedName::getOrInsert(ctx->getText(), "code");
        return CajetaType::canonicalMap[qName->toCanonical()];
    }

    cajeta::CajetaTypePtr cajeta::CajetaType::fromContext(CajetaParser::TypeTypeOrVoidContext* ctx, CajetaModulePtr module) {
        CajetaTypePtr type = nullptr;
        if (ctx != nullptr) {
            if (ctx->VOID() != nullptr) {
                QualifiedNamePtr qName = QualifiedName::getOrCreate(ctx->getText());
                type = CajetaType::canonicalMap[qName->toCanonical()];
            } else {
                type = fromContext(ctx->typeType(), module);
            }
        }
        return type;
    }

    cajeta::CajetaTypePtr cajeta::CajetaType::fromContext(CajetaParser::TypeTypeContext* ctx, CajetaModulePtr module) {
        CajetaTypePtr type;
        QualifiedNamePtr qName;
        CajetaParser::PrimitiveTypeContext* ctxPrimitiveType = ctx->primitiveType();
        if (ctxPrimitiveType != nullptr) {
            qName = QualifiedName::getOrInsert(ctxPrimitiveType->getText(), CAJETA_NATIVE_PACKAGE);
            type = canonicalMap[qName->toCanonical()];
        } else {
            CajetaParser::ClassOrInterfaceTypeContext* ctxClassOrInterface = ctx->classOrInterfaceType();
            if (ctxClassOrInterface != nullptr) {
                qName = QualifiedName::fromContext(ctxClassOrInterface);
            } else {
                throw "What is this if not a class or interface?";
            }
            if (canonicalMap.find(qName->toCanonical()) != canonicalMap.end()) {

            }
            type = canonicalMap[qName->toCanonical()];

        }
        if (ctx->LBRACK().size() > 0) {
            if (ctx->expression().size() > 0) {
                vector<long> dimensions;
                for (auto& expression : ctx->expression()) {
                    dimensions.push_back(std::stol(expression->getText()));
                }
                type = make_shared<CajetaArray>(module, type, dimensions);
            } else {
                type = make_shared<CajetaArray>(module, type, ctx->LBRACK().size());
            }
            module->getStructures()[type->toCanonical()] = static_pointer_cast<CajetaClass>(type);
            //((CajetaArray*) type)->generatePrototype();  TODO: WOT?!
        }

        return type;
    }

    CajetaTypePtr CajetaType::toPointerType() {
        QualifiedNamePtr pointerName = QualifiedName::getOrInsert(qName->getTypeName() + string("*"),
            qName->getPackageName());
        CajetaTypePtr pointerType = CajetaType::of(pointerName);
        if (!pointerType) {
            pointerType = CajetaType::create(pointerName, llvmType->getPointerTo(), POINTER_FLAG);
        }
        return pointerType;
    }

    CajetaTypePtr CajetaType::of(llvm::Type* type, CajetaTypePtr parent) {
        CajetaTypePtr result = nullptr;
        try {
            if (type->isStructTy()) {
                llvm::StringRef ref = type->getStructName();
                if (!ref.empty()) {
                    string name = ref.str();
                    result = canonicalMap[name];
                }
            } else {
                result = typeMap[TypeKey(type)];
            }
        } catch (exception) {
            throw "Exception while mapping value to type";
        }
        return result;
    }

    CajetaTypePtr CajetaType::of(llvm::Value* value, CajetaTypePtr parent) {
        return of(value->getType(), parent);
    }

    llvm::StructType* CajetaType::getOrCreateLlvmType(llvm::LLVMContext* ctx, string name) {
        llvm::StructType* result = llvm::StructType::getTypeByName(*ctx, name);
        if (result == nullptr) {
            result = llvm::StructType::create(*ctx, name);
            CajetaTypePtr type = CajetaType::create(QualifiedName::getOrCreate(name), result, STRUCT_FLAG);
            canonicalMap[name] = type;
        }
        return result;

    }

    llvm::StructType* CajetaType::getOrCreateLlvmType(llvm::LLVMContext* ctx, string name, vector<llvm::Type*> properties) {
        llvm::StructType* result = llvm::StructType::getTypeByName(*ctx, name);
        if (result == nullptr) {
            result = llvm::StructType::create(*ctx, llvm::ArrayRef<llvm::Type*>(properties), name);
            CajetaTypePtr type = CajetaType::create(QualifiedName::getOrCreate(name), result, STRUCT_FLAG);
            canonicalMap[name] = type;
        }
        return result;
    }

    /**
     *
     * @param op
     * @return
     */
    CajetaTypeFlags CajetaType::getTypeFlagsOf(llvm::Value* op) {
        unsigned long flags;
        llvm::Type* opType = op->getType();
        if (opType->getTypeID() == llvm::Type::StructTyID) {
            return STRUCT_TYPE_ID;
        } else {
            CajetaTypePtr ptr = llvmTypeIdMap[op->getType()->getTypeID()];
            return llvmTypeIdMap[opType->getTypeID()]->typeFlags;
        }
    }

    llvm::Value* CajetaType::normalize(llvm::Value* op, CajetaModulePtr module) {
        llvm::Value* result;
        CajetaTypeFlags opTypeFlags = getTypeFlagsOf(op);

        if (opTypeFlags > this->typeFlags) {
            // Throw explicit cast exception
        } else if (opTypeFlags == this->typeFlags) {
            result = op;
        } else {
            if (typeFlags & SIGNED_FLAG) {
                if (TYPE_ID(typeFlags) - TYPE_ID(opTypeFlags) == 1) {
                }
            } else { // if not signed
                if (opTypeFlags & SIGNED_FLAG) {
                    // Throw explicit cast exception
                }
            }
            switch (typeFlags) {
                case BOOLEAN_TYPE_ID:
                case UINT8_TYPE_ID:
                case UINT16_TYPE_ID:
                case UINT32_TYPE_ID:
                case UINT64_TYPE_ID:
                case UINT128_TYPE_ID:
                    result = module->getBuilder()->CreateIntCast(op, llvmType, false);
                    break;
                case INT8_TYPE_ID:
                case INT16_TYPE_ID:
                case INT32_TYPE_ID:
                case INT64_TYPE_ID:
                case INT128_TYPE_ID:
                    result = module->getBuilder()->CreateIntCast(op, llvmType, true);
                    break;
                case FLOAT16_TYPE_ID:
                case FLOAT32_TYPE_ID:
                case FLOAT64_TYPE_ID:
                case FLOAT128_TYPE_ID:
                    result = module->getBuilder()->CreateFPCast(op, llvmType);
                    break;
                default:
                    throw Exception(string("Illegal execution error, attempting to normalize non-numeric type."), string("100"));
            }
        }
        return result;
    }
}