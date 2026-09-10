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
    // A non-type template argument (the N in Vector<T, N>); never lowered to an llvm type.
    #define CONSTANT_FLAG           0b00100000000000000000
    // A fixed-width numeric vector lowering to llvm `<N x T>`. See CajetaVector.
    #define VECTOR_FLAG             0b01000000000000000000
    // A by-value POD CajetaClass declared @ValueType. Additive, and never PRIMITIVE_FLAG,
    // which is exclusive to scalar/vector/array/pointer and wired into marshalling math.
    #define VALUE_TYPE_FLAG         0b10000000000000000000
    // The STORAGE AXIS: set on a type that lives INLINE in its slot and is copied whole.
    // @ValueType PODs carry it explicitly; scalars and Vector rely on PRIMITIVE_FLAG.
    #define BY_VALUE_FLAG           0b100000000000000000000
    // `Matrix<T, R, C>` as a flat row-major llvm `<R*C x T>`, element (r,c) at lane r*C+c.
    #define MATRIX_FLAG             0b1000000000000000000000
    // Unit quaternion `Quaternion<T>` -> llvm `<4 x T>` = (w, x, y, z), w the scalar part.
    #define QUATERNION_FLAG         0b10000000000000000000000
    #define BIT_SIZE_MASK           0b00001111111000000000


    // Ordered so sub-byte floats sort below fp16: normalize() compares the full flag word.
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
    #define BFLOAT16_ID             0x0000001A00000000

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
    // bfloat16: float16's width with float32's 8-bit exponent — a distinct LLVM `bfloat`.
    #define BFLOAT16_TYPE_ID        (BFLOAT16_ID | FLOAT_FLAG | SIGNED_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_16_FLAG)
    #define FLOAT32_TYPE_ID         (FLOAT32_ID | FLOAT_FLAG | SIGNED_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_32_FLAG)
    #define FLOAT64_TYPE_ID         (FLOAT64_ID | FLOAT_FLAG | SIGNED_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_64_FLAG)
    #define FLOAT128_TYPE_ID        (FLOAT128_ID | FLOAT_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_128_FLAG)
    #define ARRAY_TYPE_ID           (STRUCT_ID | PRIMITIVE_FLAG)
    #define POINTER_TYPE_ID         (POINTER_ID | PRIMITIVE_FLAG)
    #define STRUCT_TYPE_ID          (STRUCT_ID | STRUCT_FLAG | USER_DEFINED_FLAG)
    #define TYPE_ID_MASK            0xFFFFFFFF00000000
    #define TYPE_ID(flags)          ((flags & TYPE_ID_MASK) >> 16)

    // 64-bit on every target: `unsigned long` is 32-bit on Windows (LLP64), which would
    // truncate the upper-32 _ID of every TYPE_ID and collapse distinct types together.
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

    // Source position of an enum CONSTANT — the constant registry stores only ordinals.
    struct EnumConstantPos {
        string file;
        int line = 0;
        int col = 0;
    };

class CajetaType : public Modifiable, public Annotatable,
        public std::enable_shared_from_this<CajetaType> {
    protected:
        // The per-compile type registries are thread_local, so concurrent compiles never
        // share them; resetGlobals clears the calling thread's copy each compile.
        static thread_local map<string, CajetaTypePtr> canonicalMap;
        static thread_local map<TypeKey, CajetaTypePtr> typeMap;
        static thread_local map<llvm::Type::TypeID, CajetaTypePtr> llvmTypeIdMap;

        // Where this type is DECLARED (remapped path; 1-based line, 0-based col). Here and
        // not on CajetaClass because an enum is a CajetaType; 0/"" means synthesized.
        string declaringFile;
        int declLine = 0;
        int declColumn = 0;
        // Enum-constant positions, parallel to `enumConstants`.
        static thread_local map<string, map<string, EnumConstantPos>>
            enumConstantPositions;
        // Enum constants: short type name, then constant name, to the int32 ordinal.
        // The enum's own CajetaType is registered in canonicalMap as an i32-backed type.
        static thread_local map<string, map<string, int32_t>> enumConstants;
        QualifiedNamePtr qName;
        llvm::Type* llvmType;
        // When frozen (a shared stdlib instance) the LLVM binding is a per-thread
        // side-table entry keyed by `this`, not the inline `llvmType`.
        bool frozen = false;
        // True only on the implicit class a script-shaped compilation unit synthesizes;
        // types declared INSIDE a script unit stay false. Tooling filters on it.
        bool scriptSynthesized = false;
        string canonical;
        string generic;
        CajetaTypeFlags typeFlags;
        int rank;
    public:
        void markFrozen() { frozen = true; }
        bool isFrozen() const { return frozen; }
        void setScriptSynthesized(bool v) { scriptSynthesized = v; }
        bool isScriptSynthesized() const { return scriptSynthesized; }
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

        // OR bits into the flag word, to retro-tag a type once its structure is known.
        void addTypeFlags(CajetaTypeFlags bits) {
            typeFlags |= bits;
        }

        // The robust @ValueType test: generatePrototype sets VALUE_TYPE_FLAG on the
        // CANONICAL class only, so this resolves through canonicalMap. Prefer it to a raw
        // flag test at any value-type ABI decision point.
        bool isValueType() const;

        // Storage axis, NOT the scalar axis: true for types that live INLINE in an alloca
        // and load/store whole. Both bits are born-correct on placeholders, so slot
        // allocation and POD marshalling key off THIS rather than PRIMITIVE_FLAG.
        bool hasValueSemantics() {
            return (typeFlags & PRIMITIVE_FLAG) || (typeFlags & BY_VALUE_FLAG);
        }

        QualifiedNamePtr getQName() const {
            return qName;
        }

        // Frozen-aware: the per-thread binding for a frozen object, else inline `llvmType`.
        virtual llvm::Type* getLlvmType();
        // Raw frozen-aware read of the cached binding: const, no virtual dispatch and no
        // lazy create, for the cache reads that must not take getLlvmType's branches.
        llvm::Type* rawLlvmType() const;

        // Gives a forward-referenced class a named, body-less struct type; the real
        // prototype pass calls setBody on that same struct, so references compose.
        void setLlvmType(llvm::Type* t);

        CajetaTypePtr toPointerType();

        virtual llvm::ConstantInt* getTypeAllocSize(CajetaModulePtr module);

        const string& toCanonical() {
            return qName->toCanonical();
        }

        string toGeneric();

        // Registry lookups NEVER insert on a miss: a present-but-null entry reads as
        // "already registered" and silently skips generic instantiation.
        static CajetaTypePtr of(string typeName);

        // A cheaper probe than of(): tries the raw string first, so an already-canonical
        // name costs one lookup and no interning. Same non-inserting contract.
        static CajetaTypePtr find(const string& typeName);

        // The name-keyed core of declared-type resolution — substitution, the scoped tiers,
        // archive-vouched placeholders — for sites that hold only a NAME. Null on a miss.
        static CajetaTypePtr resolveNamed(QualifiedNamePtr qName,
                                          CajetaModulePtr module);

        static CajetaTypePtr of(string typeName, string package);

        static CajetaTypePtr of(QualifiedNamePtr qName);

        // Scoped lookup for a BARE class name written in source, in fromContext's tiers:
        // own package, imports, then the last-writer-wins global short key.
        static CajetaTypePtr ofScoped(const string& shortName,
                                      CajetaModulePtr module);

        // The canonical FQN a scoped bare name denotes, in ofScoped's tiers but tolerant of
        // a FORWARD reference: a name only prescan-registered resolves. "" for a miss.
        static std::string canonicalNameScoped(const string& shortName,
                                               CajetaModulePtr module);

        // Find a TEMPLATE class under the bare `shortName`. A parameterized reference can
        // only denote a generic, so a lookup that landed a non-template re-resolves here.
        static CajetaTypePtr findTemplateByShortName(const string& shortName);

        static CajetaTypePtr of(llvm::Type* type, CajetaTypePtr parent = nullptr);

        static CajetaTypePtr of(llvm::Value* value, CajetaTypePtr parent = nullptr);

        static CajetaTypePtr fromContext(CajetaParser::PrimitiveTypeContext* ctx, CajetaModulePtr module);

        static CajetaTypePtr fromContext(CajetaParser::TypeTypeOrVoidContext* ctx, CajetaModulePtr module);

        static CajetaTypePtr fromContext(CajetaParser::TypeTypeContext* ctx, CajetaModulePtr module);

        // The resolution itself; `fromContext` wraps it to record an xref edge. Recursive
        // calls go back through the WRAPPER, so each nested name records its own token.
        static CajetaTypePtr fromContextImpl(CajetaParser::TypeTypeContext* ctx, CajetaModulePtr module);

        static map<string, CajetaTypePtr>& getCanonicalMap();

        // Every class/interface/struct the prescan saw, keyed by BOTH canonical and short
        // name so a miss can be vouched for; the value is the placeholder's canonical.
        static map<string, string>& getArchive();

        // Record one declaration the prescan found. Idempotent: a repeat leaves the
        // existing entry, and duplicate-declaration is diagnosed at compile time.
        static void registerArchive(const string& canonical,
                                    const string& shortName);

        // Mark an archive entry as an ENUM, so placeholder synthesis gives a cross-file
        // enum-typed declaration an i32-backed enum type rather than a class shell.
        static void markArchiveEnum(const string& canonical);
        static bool isArchiveEnum(const string& canonical);

        // Mark an archive entry as a VIEW: a forward reference then gets a CajetaView
        // placeholder, which view classification and member lookup dynamic_cast for.
        static void markArchiveView(const string& canonical);
        static bool isArchiveView(const string& canonical);

        // Mark an archive entry as an @ValueType class, so a cross-file declaration is
        // BORN with VALUE_TYPE_FLAG | BY_VALUE_FLAG rather than gaining them later.
        static void markArchiveValueType(const string& canonical);
        static bool isArchiveValueType(const string& canonical);

        // canonical → declaring source file, so materializeUserClass can compile that
        // module on demand. Empty when the canonical has no recorded source.
        static void registerArchiveSourcePath(const string& canonical,
                                              const string& sourcePath);
        static string lookupArchiveSourcePath(const string& canonical);

        // Mark an archive entry as an INTERFACE, so a forward-referenced interface type is
        // born a FAT `{ ptr data, ptr vtable, i64 kind }`, not a dispatch-losing thin one.
        static void markArchiveInterface(const string& canonical);
        static bool isArchiveInterface(const string& canonical);

        // Record an archived class's template metadata — `templateSource` is the literal
        // declaration text — so an early `T<args>` site can instantiate a placeholder.
        static void registerArchiveTemplate(const string& canonical,
                                            const vector<TypeParameter>& typeParameters,
                                            const string& templateSource);
        // Null when no template entry exists; the pointers live as long as the archive.
        static const vector<TypeParameter>* lookupArchiveTemplateParameters(
            const string& canonical);
        static const string* lookupArchiveTemplateSource(
            const string& canonical);

        static map<llvm::Type::TypeID, CajetaTypePtr>& getTypeIdMap();

        static void init(llvm::LLVMContext& ctxLlvm);

        // Drop every cached llvm::Type* / CajetaTypePtr, so a fresh LLVMContext cannot
        // inherit dangling pointers from a previous Compiler's destroyed context.
        static void resetGlobals();

        // Test stdlib-reuse: capture snapshots every global type container just after the
        // pristine stdlib is built, restore wipes back to it. No-ops in production.
        static void captureBaseline();
        static void restoreBaseline();
        // A SECOND baseline slot, "stdlib + the sibling sweep", restored independently on
        // a warm lint request so it skips the sweep; invalidate forces the next resweep.
        static void captureContextBaseline();
        static void restoreContextBaseline();
        static void invalidateContextBaseline();
        // Free the shared-context LLVM struct NAMES a THROWING compile left behind, so a
        // later same-named test cannot pick up a stale layout through getTypeByName.
        static void releaseThrownTransientStructNames();


        static llvm::StructType* getOrCreateLlvmType(llvm::LLVMContext* ctx, string name, vector<llvm::Type*> properties);
        static llvm::StructType* getOrCreateLlvmType(llvm::LLVMContext* ctx, string name);

        // getOrCreateLlvmType without the canonicalMap registration: the per-thread rebuild
        // must NOT re-register a plain CajetaType over the shared class/view entry.
        static llvm::StructType* getOrCreateLlvmStructNoRegister(llvm::LLVMContext* ctx, const string& name);

        static CajetaTypePtr create(QualifiedNamePtr qName) {
            CajetaTypePtr result = make_shared<CajetaType>(qName);
            // TypeKey(nullptr) dereferences, and a qName-only type has no llvmType yet.
            if (result->llvmType) typeMap[TypeKey(result->llvmType)] = result;
            result->rank = canonicalMap.size();
            canonicalMap[result->canonical] = result;

            return result;
        }

        static CajetaTypePtr create(QualifiedNamePtr qName, llvm::Type* llvmType, CajetaTypeFlags typeFlags,
            bool shareLlvmType = true) {
            CajetaTypePtr result = make_shared<CajetaType>(qName, llvmType, typeFlags);
            result->rank = canonicalMap.size();
            canonicalMap[result->canonical] = result;
            // Sub-byte/fp8 types alias an integer storage type, so registering them here
            // would clobber the canonical int; those pass shareLlvmType=false.
            if (shareLlvmType) {
                typeMap[TypeKey(result->llvmType)] = result;
                if (llvmType->getTypeID() != llvm::Type::StructTyID) {
                    llvmTypeIdMap[llvmType->getTypeID()] = result;
                }
            }
            return result;
        }

        static CajetaTypeFlags getTypeFlagsOf(llvm::Value* op);

        // Template wildcards (`<?>`), gated by the CAJETA_WILDCARDS env var or a test
        // override so the throw at the wildcard-parse site stays the default.
        static bool wildcardsEnabled();

        // Forces the wildcard flag on/off regardless of the env var; test-only, persistent.
        static void setWildcardsEnabledForTest(bool enabled);

        // Clears any test override, so wildcardsEnabled() falls back to the env var.
        static void clearWildcardsTestOverride();

        // The singleton type-identity stub for `?`, registered by init(ctx) under canonical
        // "?". Its opaque-pointer llvmType is shape only — a wildcard value is not lowered.
        static CajetaTypePtr wildcardSentinel();

        // The error/poison type: a singleton returned when resolution fails, so analysis
        // continues instead of throwing. Unregistered by name, and never lowered.
        static CajetaTypePtr error();
        bool isError() const;

        // Bounded wildcards: lazy per-(kind, bound) sentinels sharing the unbounded form's
        // opaque llvm type, with the bound itself in the wildcardBound() side table.
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

        // The bound of `? extends B` / `? super B`; null for unbounded and non-wildcards.
        CajetaTypePtr wildcardBound() const;

        // Register a per-canonical wildcard-info entry, so a capture answers the
        // wildcard-aware predicates while keeping its own identity through its qName.
        static void registerWildcardInfo(const string& canonical,
                                          WildcardKind kind,
                                          CajetaTypePtr bound);

        // Capture conversion at read positions: a static type that comes back as
        // `? extends B` projects to B, so member resolution on the result works. Other
        // kinds and wildcards nested inside generic arguments are left unchanged.
        static CajetaTypePtr captureProject(CajetaTypePtr t);

        // Called per constant when the visitor sees `enum X { A, B, C }`; the enum's own
        // CajetaType is a normal i32-backed primitive in canonicalMap.
        static void registerEnumConstant(const string& enumName,
            const string& constName, int32_t ordinal) {
            enumConstants[enumName][constName] = ordinal;
        }

        // Where this type is declared; meaningful only for a parsed declaration.
        const string& getDeclaringFile() const { return declaringFile; }
        void setDeclaringFile(const string& file) { declaringFile = file; }
        int getDeclLine() const { return declLine; }
        int getDeclColumn() const { return declColumn; }
        void setDeclPosition(int line, int column) {
            declLine = line;
            declColumn = column;
        }

        static void registerEnumConstantPosition(const string& enumName,
                const string& constName, const string& file, int line, int col) {
            enumConstantPositions[enumName][constName] = EnumConstantPos{file, line, col};
        }
        static map<string, map<string, EnumConstantPos>>& getEnumConstantPositions() {
            return enumConstantPositions;
        }
        static map<string, map<string, int32_t>>& getEnumConstants() {
            return enumConstants;
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

        /** Promote `op` to this type's rank, returning the value to use. A higher-rank
         *  operand, or a sign change that could lose data, throws instead: those need a
         *  manual cast. `module` supplies the builder for any cast emitted. */
        llvm::Value* normalize(llvm::Value* op, CajetaModulePtr module);
    };

    typedef shared_ptr<CajetaType> CajetaTypePtr;
}