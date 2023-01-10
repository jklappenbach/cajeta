//
// Created by James Klappenbach on 10/2/22.
//

#pragma once

#include "Modifiable.h"
#include "Annotatable.h"
#include "QualifiedName.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/ADT/StringRef.h"

using namespace std;

namespace cajeta {
    #define PRIMITIVE_FLAG          0b0000000000000001
    #define NUMBER_FLAG             0b0000000000000010
    #define INT_FLAG                0b0000000000000100
    #define FLOAT_FLAG              0b0000000000001000
    #define SIGNED_FLAG             0b0000000000010000
    #define STRUCT_FLAG             0b0000000000100000
    #define POINTER_FLAG            0b0000000001000000
    #define REFERENCE_FLAG          0b0000000010000000
    #define USER_DEFINED_FLAG       0b0000000100000000
    #define BIT_8_FLAG              0b0000001000000000
    #define BIT_16_FLAG             0b0000010000000000
    #define BIT_32_FLAG             0b0000100000000000
    #define BIT_64_FLAG             0b0001000000000000
    #define BIT_128_FLAG            0b0010000000000000
    #define BIT_SIZE_MASK           0b0011111000000000


    #define VOID_ID                 0x0000000100000000
    #define BOOLEAN_ID              0x0000000200000000
    #define UINT8_ID                0x0000000300000000
    #define INT8_ID                 0x0000000400000000
    #define UINT16_ID               0x0000000500000000
    #define INT16_ID                0x0000000600000000
    #define UINT32_ID               0x0000000700000000
    #define INT32_ID                0x0000000800000000
    #define UINT64_ID               0x0000000900000000
    #define INT64_ID                0x0000000A00000000
    #define UINT128_ID              0x0000000B00000000
    #define INT128_ID               0x0000000C00000000
    #define FLOAT16_ID              0x0000000D00000000
    #define FLOAT32_ID              0x0000000E00000000
    #define FLOAT64_ID              0x0000000F00000000
    #define FLOAT128_ID             0x0000001000000000
    #define POINTER_ID              0x0000001100000000
    #define STRUCT_ID               0x0000001200000000

    #define VOID_TYPE_ID            (VOID_ID | PRIMITIVE_FLAG)
    #define BOOLEAN_TYPE_ID         (BOOLEAN_ID | INT_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG)
    #define UINT8_TYPE_ID           (UINT8_ID | INT_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_8_FLAG)
    #define INT8_TYPE_ID            (INT8_ID | INT_FLAG | SIGNED_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_8_FLAG)
    #define UINT16_TYPE_ID          (UINT16_ID | INT_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_16_FLAG)
    #define INT16_TYPE_ID           (INT16_ID | INT_FLAG | SIGNED_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_16_FLAG)
    #define UINT32_TYPE_ID          (UINT32_ID | INT_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_32_FLAG)
    #define INT32_TYPE_ID           (INT32_ID | INT_FLAG | SIGNED_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_32_FLAG)
    #define UINT64_TYPE_ID          (UINT64_ID | INT_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_64_FLAG)
    #define INT64_TYPE_ID           (INT64_ID | INT_FLAG | SIGNED_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_64_FLAG)
    #define UINT128_TYPE_ID         (UINT128_ID | INT_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_128_FLAG)
    #define INT128_TYPE_ID          (INT128_ID | INT_FLAG | SIGNED_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_128_FLAG)
    #define FLOAT16_TYPE_ID         (FLOAT16_ID | FLOAT_FLAG | SIGNED_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_16_FLAG)
    #define FLOAT32_TYPE_ID         (FLOAT32_ID | FLOAT_FLAG | SIGNED_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_32_FLAG)
    #define FLOAT64_TYPE_ID         (FLOAT64_ID | FLOAT_FLAG | SIGNED_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_64_FLAG)
    #define FLOAT128_TYPE_ID        (FLOAT128_ID | FLOAT_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_128_FLAG)
    #define ARRAY_TYPE_ID           (STRUCT_ID | PRIMITIVE_FLAG)
    #define POINTER_TYPE_ID         (POINTER_ID | PRIMITIVE_FLAG)
    #define STRUCT_TYPE_ID          (STRUCT_ID | STRUCT_FLAG | USER_DEFINED_FLAG)
    #define TYPE_ID_MASK            0xFFFFFFFF00000000
    #define TYPE_ID(flags)          ((flags & TYPE_ID_MASK) >> 16)

    typedef unsigned long CajetaTypeFlags;

    class Method;
    typedef shared_ptr<Method> MethodPtr;

    class Field;
    typedef shared_ptr<Field> FieldPtr;

    class CajetaModule;
    typedef shared_ptr<CajetaModule> CajetaModulePtr;

    class CajetaType;
    typedef shared_ptr<CajetaType> CajetaTypePtr;

    struct TypeKey {
        int typeId;
        int typeCode;

        TypeKey(llvm::Type* type);
    };

    bool operator<(const TypeKey& a, const TypeKey& b);

class CajetaType : public Modifiable, public Annotatable,
        public std::enable_shared_from_this<CajetaType> {
    protected:
        static map<string, CajetaTypePtr> canonicalMap;
        static map<TypeKey, CajetaTypePtr> typeMap;
        static map<llvm::Type::TypeID, CajetaTypePtr> llvmTypeIdMap;
        QualifiedNamePtr qName;
        llvm::Type* llvmType;
        string canonical;
        string generic;
        CajetaTypeFlags typeFlags;
        int rank;
    public:
        CajetaType() {
            this->typeFlags = STRUCT_FLAG;
            llvmType = nullptr;
        }

        CajetaType(QualifiedNamePtr qName) {
            this->typeFlags = STRUCT_FLAG;
            this->qName = qName;
            canonical = qName->toCanonical();
            generic = toGeneric();
            llvmType = nullptr;
        }

        CajetaType(string typeName, llvm::Type* llvmType, CajetaTypeFlags typeFlags) {
            qName = QualifiedName::getOrCreate(typeName);
            this->llvmType = llvmType;
            this->typeFlags = typeFlags;
        }

        CajetaType(QualifiedNamePtr qName, llvm::Type* llvmType, CajetaTypeFlags typeFlags) {
            this->qName = qName;
            this->llvmType = llvmType;
            this->typeFlags = typeFlags;
            canonical = qName->toCanonical();
        }

        CajetaType(const CajetaType& src) {
            typeFlags = src.typeFlags;
            qName = src.qName;
            llvmType = src.llvmType;
            canonical = src.canonical;
        }

    public:
        int getRank() { return rank; }

        virtual CajetaTypeFlags getTypeFlags() {
            return typeFlags;
        }

        QualifiedNamePtr getQName() const {
            return qName;
        }

        virtual llvm::Type* getLlvmType() {
            return llvmType;
        }

        CajetaTypePtr toPointerType();

        virtual llvm::ConstantInt* getTypeAllocSize(CajetaModulePtr module);

        const string& toCanonical() {
            return qName->toCanonical();
        }

        string toGeneric();

        static CajetaTypePtr of(string typeName);

        static CajetaTypePtr of(string typeName, string package);

        static CajetaTypePtr of(QualifiedNamePtr qName);

        static CajetaTypePtr fromContext(CajetaParser::PrimitiveTypeContext* ctx, CajetaModulePtr module);

        static CajetaTypePtr fromContext(CajetaParser::TypeTypeOrVoidContext* ctx, CajetaModulePtr module);

        static CajetaTypePtr fromContext(CajetaParser::TypeTypeContext* ctx, CajetaModulePtr module);

        static map<string, CajetaTypePtr>& getCanonicalMap();

        static map<llvm::Type::TypeID, CajetaTypePtr>& getTypeIdMap();

        static void init(llvm::LLVMContext& ctxLlvm);

        static CajetaTypePtr of(llvm::Type* type, CajetaTypePtr parent = nullptr);

        static CajetaTypePtr of(llvm::Value* value, CajetaTypePtr parent = nullptr);

        static llvm::StructType* getOrCreateLlvmType(llvm::LLVMContext* ctx, string name, vector<llvm::Type*> properties);
        static llvm::StructType* getOrCreateLlvmType(llvm::LLVMContext* ctx, string name);

        static CajetaTypePtr create() {
            return CajetaType::create();
        }

        static CajetaTypePtr create(QualifiedNamePtr qName) {
            CajetaTypePtr result = make_shared<CajetaType>(qName);
            typeMap[TypeKey(result->llvmType)] = result;
            result->rank = canonicalMap.size();
            canonicalMap[result->canonical] = result;

            return result;
        }

        static CajetaTypePtr create(QualifiedNamePtr qName, llvm::Type* llvmType, CajetaTypeFlags typeFlags) {
            CajetaTypePtr result = make_shared<CajetaType>(qName, llvmType, typeFlags);
            typeMap[TypeKey(result->llvmType)] = result;
            result->rank = canonicalMap.size();
            canonicalMap[result->canonical] = result;
            if (llvmType->getTypeID() != llvm::Type::StructTyID) {
                llvmTypeIdMap[llvmType->getTypeID()] = result;
            }
            return result;
        }

        static unsigned long getTypeFlagsOf(llvm::Value* op);

        /**
         *  Sources for both LHS and RHS:
         *  - Field (Value*)
         *  - Literal (Constant*)
         *  - Return value
         *
         *  Use cases:
         *
         *  - If values are numeric:
         *      - If equal types, return the original value
         *      - If ap is a higher rank, throw an exception requiring manual cast
         *      - If op is equal lesser rank:
         *          - If op is signed and src is unsigned, throw an exception requiring manual cast (potential loss of data)
         *          - If op is unsigned and src is signed, throw a warning (integer overflow), but allow if rank delta is 1.  Otherwise,
         *
         * @param op The Value* to be compared against this value
         * @param module The module to use for creating builder statement to cast the argument to a new value with rank parity
         * @return A normalize result with any warnings to promote
         */
        llvm::Value* normalize(llvm::Value* op, CajetaModulePtr module);
    };

    typedef shared_ptr<CajetaType> CajetaTypePtr;
}