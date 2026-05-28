//
// Created by James Klappenbach on 10/2/22.
//

#pragma once

#include "Modifiable.h"
#include "Annotatable.h"
#include "QualifiedName.h"
#include "Templates.h"
#include <cstdint>
#include <optional>
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include <llvm/TargetParser/Host.h>
#include "llvm/Support/TargetSelect.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/ADT/StringRef.h"

using namespace std;

namespace cajeta {
    #define PRIMITIVE_FLAG          0b00000000000000000001
    #define NUMBER_FLAG             0b00000000000000000010
    #define INT_FLAG                0b00000000000000000100
    #define FLOAT_FLAG              0b00000000000000001000
    #define SIGNED_FLAG             0b00000000000000010000
    #define STRUCT_FLAG             0b00000000000000100000
    #define POINTER_FLAG            0b00000000000001000000
    #define REFERENCE_FLAG          0b00000000000010000000
    #define USER_DEFINED_FLAG       0b00000000000100000000
    #define BIT_4_FLAG              0b00000000001000000000
    #define BIT_6_FLAG              0b00000000010000000000
    #define BIT_8_FLAG              0b00000000100000000000
    #define BIT_16_FLAG             0b00000001000000000000
    #define BIT_32_FLAG             0b00000010000000000000
    #define BIT_64_FLAG             0b00000100000000000000
    #define BIT_128_FLAG            0b00001000000000000000
    #define ENUM_FLAG               0b00010000000000000000
    #define BIT_SIZE_MASK           0b00001111111000000000


    // Numeric IDs are ordered so sub-byte floats sort below fp16; CajetaType::normalize()
    // compares the full flag word, so an fp4/fp6/fp8 operand normalizes up to fp16/fp32/etc.
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
    #define FLOAT4E2M1_ID           0x0000000D00000000
    #define FLOAT6E2M3_ID           0x0000000E00000000
    #define FLOAT6E3M2_ID           0x0000000F00000000
    #define FLOAT8E4M3_ID           0x0000001000000000
    #define FLOAT8E5M2_ID           0x0000001100000000
    #define FLOAT8E4M3FNUZ_ID       0x0000001200000000
    #define FLOAT8E5M2FNUZ_ID       0x0000001300000000
    #define FLOAT16_ID              0x0000001400000000
    #define FLOAT32_ID              0x0000001500000000
    #define FLOAT64_ID              0x0000001600000000
    #define FLOAT128_ID             0x0000001700000000
    #define POINTER_ID              0x0000001800000000
    #define STRUCT_ID               0x0000001900000000

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
    #define FLOAT4E2M1_TYPE_ID      (FLOAT4E2M1_ID | FLOAT_FLAG | SIGNED_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_4_FLAG)
    #define FLOAT6E2M3_TYPE_ID      (FLOAT6E2M3_ID | FLOAT_FLAG | SIGNED_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_6_FLAG)
    #define FLOAT6E3M2_TYPE_ID      (FLOAT6E3M2_ID | FLOAT_FLAG | SIGNED_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_6_FLAG)
    #define FLOAT8E4M3_TYPE_ID      (FLOAT8E4M3_ID | FLOAT_FLAG | SIGNED_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_8_FLAG)
    #define FLOAT8E5M2_TYPE_ID      (FLOAT8E5M2_ID | FLOAT_FLAG | SIGNED_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_8_FLAG)
    #define FLOAT8E4M3FNUZ_TYPE_ID  (FLOAT8E4M3FNUZ_ID | FLOAT_FLAG | SIGNED_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_8_FLAG)
    #define FLOAT8E5M2FNUZ_TYPE_ID  (FLOAT8E5M2FNUZ_ID | FLOAT_FLAG | SIGNED_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_8_FLAG)
    #define FLOAT16_TYPE_ID         (FLOAT16_ID | FLOAT_FLAG | SIGNED_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_16_FLAG)
    #define FLOAT32_TYPE_ID         (FLOAT32_ID | FLOAT_FLAG | SIGNED_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_32_FLAG)
    #define FLOAT64_TYPE_ID         (FLOAT64_ID | FLOAT_FLAG | SIGNED_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_64_FLAG)
    #define FLOAT128_TYPE_ID        (FLOAT128_ID | FLOAT_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_128_FLAG)
    #define ARRAY_TYPE_ID           (STRUCT_ID | PRIMITIVE_FLAG)
    #define POINTER_TYPE_ID         (POINTER_ID | PRIMITIVE_FLAG)
    #define STRUCT_TYPE_ID          (STRUCT_ID | STRUCT_FLAG | USER_DEFINED_FLAG)
    #define TYPE_ID_MASK            0xFFFFFFFF00000000
    #define TYPE_ID(flags)          ((flags & TYPE_ID_MASK) >> 16)

    // 64-bit on every supported target. `unsigned long` is 32-bit on Windows
    // (LLP64), which silently truncates the upper-32 _ID component of every
    // TYPE_ID and collapses distinct types onto the same numeric value.
    typedef uint64_t CajetaTypeFlags;

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
        // Enum constant registry. Keyed by the enum's short typeName
        // ("Direction") and then by constant name ("NORTH" / "SOUTH" / ...).
        // The value is the constant's int32 ordinal. DotExpression consults
        // this for `MyEnum.CONST` references; the enum CajetaType itself
        // is registered in canonicalMap as an i32-backed type.
        static map<string, map<string, int32_t>> enumConstants;
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

        // Used by the placeholder-synthesis path so a forward-
        // referenced class has a named (body-less) struct type
        // before its real generatePrototype runs. The real pass
        // calls setBody on the same struct (getOrCreateLlvmType
        // is canonical-keyed) so existing references compose
        // correctly.
        void setLlvmType(llvm::Type* t) { llvmType = t; }

        CajetaTypePtr toPointerType();

        virtual llvm::ConstantInt* getTypeAllocSize(CajetaModulePtr module);

        const string& toCanonical() {
            return qName->toCanonical();
        }

        string toGeneric();

        static CajetaTypePtr of(string typeName);

        static CajetaTypePtr of(string typeName, string package);

        static CajetaTypePtr of(QualifiedNamePtr qName);

        static CajetaTypePtr of(llvm::Type* type, CajetaTypePtr parent = nullptr);

        static CajetaTypePtr of(llvm::Value* value, CajetaTypePtr parent = nullptr);

        static CajetaTypePtr fromContext(CajetaParser::PrimitiveTypeContext* ctx, CajetaModulePtr module);

        static CajetaTypePtr fromContext(CajetaParser::TypeTypeOrVoidContext* ctx, CajetaModulePtr module);

        static CajetaTypePtr fromContext(CajetaParser::TypeTypeContext* ctx, CajetaModulePtr module);

        static map<string, CajetaTypePtr>& getCanonicalMap();

        // Archive of class/interface/struct declarations available in
        // the current compilation unit. Populated by a pre-scan over
        // every .cajeta source under the source root (or, for the
        // multi-source JIT helper, every source string the test
        // provided) BEFORE any visitor walks begin. Keyed by both
        // canonical name (`pkg.Class`) and short typeName (`Class`)
        // so fromContext's miss path can vouch for a referenced name
        // before deciding to create a placeholder vs throw.
        //
        // The mapped value is the resolved canonical the placeholder
        // would be created under — short-name lookups carry the full
        // qualified name from the archive so we don't pollute
        // canonicalMap with bare-name entries that collide across
        // packages.
        static map<string, string>& getArchive();

        // Record one class/interface/struct declaration found by the
        // pre-scan. Idempotent — repeated registration of the same
        // canonical leaves the existing entry. Same canonical from a
        // second source file is treated as a duplicate-declaration
        // error at compile time (not here — the archive just notes
        // first-sight).
        static void registerArchive(const string& canonical,
                                    const string& shortName);

        // Mark a previously-registered archive entry as an enum
        // declaration (not a class / interface / struct / view).
        // Read by fromContext's placeholder-synthesis path so cross-
        // file enum-typed field declarations resolve to an i32-
        // backed enum CajetaType rather than a class placeholder.
        // Called by the prescan visitor's visitEnumDeclaration after
        // registerArchive(canonical, shortName).
        static void markArchiveEnum(const string& canonical);
        static bool isArchiveEnum(const string& canonical);

        // Record template metadata for an archived class. Called by
        // the prescan visitor for any class/interface declaration
        // that carries a `typeParameters` clause. The templateSource
        // is the literal text of the enclosing typeDeclaration —
        // mirroring what visitClassDeclaration captures at parse
        // time. Lets fromContext's placeholder-synthesis path
        // pre-set typeParameters + templateSource on a placeholder
        // so an early `T<args>` use site can instantiate before
        // the real visitClassDeclaration runs.
        static void registerArchiveTemplate(const string& canonical,
                                            const vector<TypeParameter>& typeParameters,
                                            const string& templateSource);
        // Read accessors — return null if no template entry exists
        // for `canonical`. Pointers stay valid for the lifetime of
        // the static archive map.
        static const vector<TypeParameter>* lookupArchiveTemplateParameters(
            const string& canonical);
        static const string* lookupArchiveTemplateSource(
            const string& canonical);

        static map<llvm::Type::TypeID, CajetaTypePtr>& getTypeIdMap();

        static void init(llvm::LLVMContext& ctxLlvm);

        // Drop all cached llvm::Type* / CajetaTypePtr entries. Called from the Compiler
        // constructor before init() so a fresh LLVMContext doesn't inherit dangling
        // pointers from a previous Compiler's now-destroyed context.
        static void resetGlobals();


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

        static CajetaTypePtr create(QualifiedNamePtr qName, llvm::Type* llvmType, CajetaTypeFlags typeFlags,
            bool shareLlvmType = true) {
            CajetaTypePtr result = make_shared<CajetaType>(qName, llvmType, typeFlags);
            result->rank = canonicalMap.size();
            canonicalMap[result->canonical] = result;
            // Sub-byte/fp8 types alias an integer storage type (i4/i6/i8); registering them in
            // typeMap or llvmTypeIdMap would clobber the canonical int registration. Pass
            // shareLlvmType=false in that case.
            if (shareLlvmType) {
                typeMap[TypeKey(result->llvmType)] = result;
                if (llvmType->getTypeID() != llvm::Type::StructTyID) {
                    llvmTypeIdMap[llvmType->getTypeID()] = result;
                }
            }
            return result;
        }

        static CajetaTypeFlags getTypeFlagsOf(llvm::Value* op);

        // Template wildcards (`<?>`) — Step 1 foundation. Gated by the
        // CAJETA_WILDCARDS env var (or a test override) so the existing
        // throw at the wildcard-parse site stays the default while the
        // foundation lands. Rationale, costs, and full staging plan
        // live in cajeta-docs/TemplateWildcard.md and todo.md.
        static bool wildcardsEnabled();

        // Forces the wildcard flag on/off regardless of the env var.
        // Test-only entry point — production callers should use the
        // env var. Persists until cleared.
        static void setWildcardsEnabledForTest(bool enabled);

        // Clears any test override. Subsequent calls to
        // wildcardsEnabled() fall back to the env-var check.
        static void clearWildcardsTestOverride();

        // Singleton type-identity stub for the unbounded wildcard `?`.
        // Registered in canonicalMap under canonical "?" by init(ctx)
        // so wildcardSentinel() is non-null in any Compiler-bootstrapped
        // process. Carries an opaque-pointer llvmType purely for shape
        // — codegen on a wildcard-typed value is NOT yet supported
        // (Step 2 lands the drop-chain ABI). Step 1 wires the sentinel
        // through parsing and the template-instantiation cache only.
        static CajetaTypePtr wildcardSentinel();

        // Step 6 — bounded wildcards. Lazy per-(kind, bound) sentinels
        // cached in canonicalMap under canonicals "? extends <bound>"
        // and "? super <bound>". All wildcard sentinels share the
        // unbounded form's llvm type (opaque pointer). The bound is
        // recorded in a side table queried via wildcardBound().
        static CajetaTypePtr wildcardSentinelExtends(CajetaTypePtr bound);
        static CajetaTypePtr wildcardSentinelSuper(CajetaTypePtr bound);

        // Wildcard kind classification.
        enum class WildcardKind {
            None,        // not a wildcard
            Unbounded,   // `?`
            Extends,     // `? extends Bound`
            Super        // `? super Bound`
        };

        // True iff this is any wildcard sentinel (unbounded or bounded).
        bool isWildcard() const;

        // Wildcard kind classifier. Returns None for non-wildcards.
        WildcardKind wildcardKind() const;

        // Bound type for `? extends Bound` / `? super Bound`. Returns
        // null for unbounded wildcards and non-wildcards.
        CajetaTypePtr wildcardBound() const;

        // Register a per-canonical wildcard-info entry. Used by
        // CajetaCapture's factories — captures need to appear as
        // wildcards to existing wildcard-aware code paths (isWildcard,
        // wildcardKind, wildcardBound, isWildcardInstantiation,
        // substitution-stable hash machinery) while retaining their
        // own per-binding identity via the capture's unique qName.
        static void registerWildcardInfo(const string& canonical,
                                          WildcardKind kind,
                                          CajetaTypePtr bound);

        // Capture conversion projection at read positions
        // (cajeta-docs/TemplateWildcard.md § 3 Capture identity). When
        // an expression's static type comes back from method
        // resolution as a bounded-extends wildcard — typically the
        // return type of a method on a `Foo<? extends B>` receiver —
        // the caller should see the bound `B`, not the raw sentinel,
        // so member-resolution on the result works.
        //
        // Scope (v1):
        //   - `? extends B` → B
        //   - other wildcard kinds + non-wildcards → unchanged
        //   - nested wildcards inside generic args (`Foo<? extends B>`)
        //     left alone; variance through type constructors needs
        //     proper capture-identity tracking.
        static CajetaTypePtr captureProject(CajetaTypePtr t);

        // Enum support. `registerEnumConstant` is called per constant when
        // the visitor sees `enum X { A, B, C }`. `lookupEnumConstant` returns
        // the int32 ordinal if `enumName.constName` is a registered enum
        // constant; `nullopt` otherwise. The enum's CajetaType itself is
        // a normal i32-backed primitive registered in canonicalMap.
        static void registerEnumConstant(const string& enumName,
            const string& constName, int32_t ordinal) {
            enumConstants[enumName][constName] = ordinal;
        }
        static bool isEnumName(const string& enumName) {
            return enumConstants.find(enumName) != enumConstants.end();
        }
        static const std::optional<int32_t> lookupEnumConstant(
            const string& enumName, const string& constName) {
            auto it = enumConstants.find(enumName);
            if (it == enumConstants.end()) return std::nullopt;
            auto cit = it->second.find(constName);
            if (cit == it->second.end()) return std::nullopt;
            return cit->second;
        }

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
         * @param module The pModule to use for creating builder statement to cast the argument to a new value with rank parity
         * @return A normalize result with any warnings to promote
         */
        llvm::Value* normalize(llvm::Value* op, CajetaModulePtr module);
    };

    typedef shared_ptr<CajetaType> CajetaTypePtr;
}