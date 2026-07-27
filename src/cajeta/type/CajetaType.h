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
    // A compile-time integer constant carried as a non-type template argument
    // (the N in Vector<T, N>) — never lowered to an llvm type. See CajetaConstantType.
    #define CONSTANT_FLAG           0b00100000000000000000
    // A fixed-width numeric vector lowering to llvm `<N x T>`. See CajetaVector.
    #define VECTOR_FLAG             0b01000000000000000000
    // A by-value POD CajetaClass declared @ValueType: eligible for operator-overload
    // dispatch (the !PRIMITIVE_FLAG gate is relaxed for it) while still marshalling by
    // value. Additive — NOT PRIMITIVE_FLAG (which is exclusive to scalar/vector/array/
    // pointer and is wired into width/marshalling math). See plans/value-type-overloading-plan.md.
    #define VALUE_TYPE_FLAG         0b10000000000000000000
    // The STORAGE AXIS, orthogonal to the scalar/kind axes above. Set on every
    // type that lives INLINE in its slot and is copied whole (load/store the
    // aggregate, no heap body, no drop/borrow): @ValueType PODs carry it
    // EXPLICITLY (they are not PRIMITIVE_FLAG). Scalar primitives and Vector are
    // by-value too, but already reliably marked PRIMITIVE_FLAG, so hasValueSemantics()
    // tests both bits rather than retro-tagging every numeric. A future builtin
    // by-value type (Matrix/Tensor) that does NOT borrow PRIMITIVE_FLAG sets this
    // bit alone. Born-correct on cross-file placeholders via markArchiveValueType.
    #define BY_VALUE_FLAG           0b100000000000000000000
    // A fixed-shape numeric matrix `Matrix<T, R, C>` lowering to a flat
    // row-major llvm `<R*C x T>` (element (r,c) = lane r*C+c). Like VECTOR_FLAG
    // it rides PRIMITIVE_FLAG for by-value marshalling; the dedicated matrix
    // codegen path (construction, m[r][c], element-wise, * = matmul) recognizes
    // it. See CajetaMatrix and plans/fluttering-sparking-lantern.md (B1).
    #define MATRIX_FLAG             0b1000000000000000000000
    // Unit quaternion `Quaternion<T>` -> llvm `<4 x T>` = (w, x, y, z), w the
    // scalar part. Like VECTOR_FLAG it rides PRIMITIVE_FLAG for by-value
    // marshalling; the dedicated quaternion codegen path (construction, `*` =
    // Hamilton product / vector rotation, normalize/conjugate/slerp) recognizes
    // it. See CajetaQuaternion.
    #define QUATERNION_FLAG         0b10000000000000000000000
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
    // bfloat16 (brain float): same 16-bit width as float16 but a wider exponent
    // (8-bit, like float32) — distinct LLVM `bfloat`. The ML training dtype.
    #define BFLOAT16_TYPE_ID        (BFLOAT16_ID | FLOAT_FLAG | SIGNED_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG | BIT_16_FLAG)
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

    // Source position of an enum CONSTANT. The enum-constant registry stores only
    // the ordinal, so without this an IDE could find `Color` but not `Color.GREEN`
    // — Ctrl-click on a constant would land on the enum, or on nothing.
    // See specs/ide-symbol-index-spec.md §2.
    struct EnumConstantPos {
        string file;
        int line = 0;
        int col = 0;
    };

class CajetaType : public Modifiable, public Annotatable,
        public std::enable_shared_from_this<CajetaType> {
    protected:
        // thread-safe-compiler Unit 2: the per-compile type registries are
        // thread_local so concurrent compiles on different threads never share
        // them. Single-threaded behavior is unchanged (resetGlobals clears the
        // calling thread's copy each compile). Units 5-6 split these into a
        // shared frozen-stdlib tier + a per-thread user tier.
        static thread_local map<string, CajetaTypePtr> canonicalMap;
        static thread_local map<TypeKey, CajetaTypePtr> typeMap;
        static thread_local map<llvm::Type::TypeID, CajetaTypePtr> llvmTypeIdMap;

        // Where this type is DECLARED (remapped path; 1-based line, 0-based col —
        // the ANTLR convention). Lives on CajetaType rather than CajetaClass because
        // an ENUM is a CajetaType (i32-backed, ENUM_FLAG) and not a CajetaClass, so
        // a class-only field left every enum unlocatable. 0/"" = synthesized (mock,
        // template placeholder, primitive) — such a type has no source an IDE could
        // open, and the xref export skips it rather than emit a record pointing
        // nowhere. See specs/ide-symbol-index-spec.md §2.
        string declaringFile;
        int declLine = 0;
        int declColumn = 0;
        // Enum-constant positions, parallel to `enumConstants`. See
        // registerEnumConstantPosition().
        static thread_local map<string, map<string, EnumConstantPos>>
            enumConstantPositions;
        // Enum constant registry. Keyed by the enum's short typeName
        // ("Direction") and then by constant name ("NORTH" / "SOUTH" / ...).
        // The value is the constant's int32 ordinal. DotExpression consults
        // this for `MyEnum.CONST` references; the enum CajetaType itself
        // is registered in canonicalMap as an i32-backed type.
        static thread_local map<string, map<string, int32_t>> enumConstants;
        QualifiedNamePtr qName;
        llvm::Type* llvmType;
        // threadsafe U6: when frozen (a shared stdlib instance), the LLVM binding
        // is NOT the inline `llvmType` (one context) but a per-thread side-table
        // entry keyed by `this`, so threads with different LLVMContexts each get
        // their own binding for the one shared object. Default false → inline
        // (unchanged behavior) until the stdlib is frozen in 6.4.
        bool frozen = false;
        string canonical;
        string generic;
        CajetaTypeFlags typeFlags;
        int rank;
    public:
        void markFrozen() { frozen = true; }
        bool isFrozen() const { return frozen; }
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

        // OR additional bits into the flag word. Used to retro-tag an already
        // built type — e.g. a class declared `@ValueType` gains VALUE_TYPE_FLAG
        // after its structure is known. See plans/value-type-overloading-plan.md.
        void addTypeFlags(CajetaTypeFlags bits) {
            typeFlags |= bits;
        }

        // Robust @ValueType test (plans/value-type-overloading-plan.md S2).
        // VALUE_TYPE_FLAG is applied to the CANONICAL CajetaClass inside
        // generatePrototype, but consumer sites (parse-time placeholders,
        // un-refreshed local-variable type instances) may hold a DIFFERENT
        // CajetaType object for the same class that never received the bit.
        // Resolving through canonicalMap by canonical name makes every
        // instance of a value-type class answer true once the class is
        // prototyped. Prefer this over a raw `getTypeFlags() & VALUE_TYPE_FLAG`
        // at any value-type ABI decision point.
        bool isValueType() const;

        // Storage axis (NOT the scalar axis). True for types with value /
        // Copy semantics that live INLINE in an alloca and are loaded/stored
        // whole: scalar primitives (ints/floats — PRIMITIVE_FLAG), the
        // by-value device types (Vector/CooperativeMatrix, which borrow
        // PRIMITIVE_FLAG for kernel-arg marshalling), and @ValueType PODs
        // (BY_VALUE_FLAG — they are NOT primitives, and must not be, or they'd
        // fail the `!(PRIMITIVE_FLAG)` operator-dispatch gate). The two bits
        // together span the storage axis: PRIMITIVE_FLAG already marks every
        // scalar/vector by-value type uniformly, BY_VALUE_FLAG marks the
        // non-primitive ones. Both are born-correct on cross-file placeholders
        // (PRIMITIVE_FLAG always, BY_VALUE_FLAG via markArchiveValueType), so
        // this is a reliable direct flag test with no canonical-map backstop.
        // Slot allocation, by-value load/store, and POD kernel marshalling key
        // off THIS, not the scalar bit. See plans/value-type-overloading-plan.md.
        bool hasValueSemantics() {
            return (typeFlags & PRIMITIVE_FLAG) || (typeFlags & BY_VALUE_FLAG);
        }

        QualifiedNamePtr getQName() const {
            return qName;
        }

        // Frozen-aware: returns the per-thread binding for a frozen (shared
        // stdlib) object, else the inline `llvmType`. Out-of-line so it can reach
        // the thread_local binding table (CajetaType.cpp). (threadsafe U6.1)
        virtual llvm::Type* getLlvmType();
        // Raw frozen-aware read of the cached binding: const, NO virtual dispatch
        // and NO lazy-create. Subclasses + const methods use this for cache reads
        // (the virtual getLlvmType has placeholder/wildcard branches). (U6.2)
        llvm::Type* rawLlvmType() const;

        // Used by the placeholder-synthesis path so a forward-
        // referenced class has a named (body-less) struct type
        // before its real generatePrototype runs. The real pass
        // calls setBody on the same struct (getOrCreateLlvmType
        // is canonical-keyed) so existing references compose
        // correctly.
        void setLlvmType(llvm::Type* t);

        CajetaTypePtr toPointerType();

        virtual llvm::ConstantInt* getTypeAllocSize(CajetaModulePtr module);

        const string& toCanonical() {
            return qName->toCanonical();
        }

        string toGeneric();

        static CajetaTypePtr of(string typeName);

        // Read-only lookup: like of(), but NEVER touches the registry on a
        // miss. of() indexes canonicalMap with operator[], so probing a name
        // that does not exist INSERTS a null entry under it — after which
        // "already registered?" checks see a present-but-null type and the
        // generic-instantiation machinery skips generating it (the failure
        // mode is `Symbols not found: Foo<Bar>#ClassObject` in a later
        // session sharing the type world). Use this anywhere a name may
        // legitimately not resolve — probing, introspection, diagnostics.
        static CajetaTypePtr find(const string& typeName);

        // The name-keyed core of declared-type resolution: substitution,
        // scoped tiers (own package -> imports -> global), archive-vouched
        // placeholder synthesis. For resolution sites that hold only a NAME
        // (no parser context) — resolves identically to a declared type.
        // Null when the name resolves nowhere; the caller owns the miss.
        static CajetaTypePtr resolveNamed(QualifiedNamePtr qName,
                                          CajetaModulePtr module);

        static CajetaTypePtr of(string typeName, string package);

        static CajetaTypePtr of(QualifiedNamePtr qName);

        // Scoped short-name lookup for a BARE class name written in source
        // (class-name receivers of static calls, static-field LHS, bare
        // allocations). Mirrors fromContext's bare-name tier order:
        //   1. the module's own package,
        //   2. the module's explicit imports — when the import names the
        //      class but its canonical isn't materialized yet, returns
        //      nullptr rather than letting the global key answer wrongly,
        //   3. the global canonical/short-name key (legacy behavior).
        // The global short key is last-writer-wins across packages, so a
        // same-named class elsewhere (e.g. a user class shadowing a stdlib
        // name) poisons any call site that consults it directly; resolve
        // source-written bare names through here instead.
        static CajetaTypePtr ofScoped(const string& shortName,
                                      CajetaModulePtr module);

        // The canonical FQN a scoped bare name denotes, as a STRING — mirroring
        // ofScoped's tiers (own package → imports → global) but tolerant of a
        // FORWARD reference: a name that is only prescan-registered (in the
        // archive) and not yet built into canonicalMap still resolves. Used by
        // the xref reference capture for `heap Point(...)` created types, where
        // the whole-root export's directory-order parse routinely reaches an
        // allocation before its target's declaration is built. Returns "" when
        // the name names nothing known. Position-free — the caller supplies it.
        static std::string canonicalNameScoped(const string& shortName,
                                               CajetaModulePtr module);

        // Find a generic (template) class registered under the bare short
        // name `shortName`, scanning the process-global canonicalMap. Used to
        // recover from same-short-name collisions: a parameterized reference
        // `Foo<...>` can only denote a generic class, so when an ordinary
        // name lookup lands a NON-template (e.g. `Stream` resolving to the
        // final, non-generic cajeta.xpu.KernelStream instead of the generic
        // cajeta.lang.stream.Stream because both register the bare key
        // "Stream" with last-writer-wins), callers re-resolve through here.
        // Returns nullptr when no same-short-name template exists.
        static CajetaTypePtr findTemplateByShortName(const string& shortName);

        static CajetaTypePtr of(llvm::Type* type, CajetaTypePtr parent = nullptr);

        static CajetaTypePtr of(llvm::Value* value, CajetaTypePtr parent = nullptr);

        static CajetaTypePtr fromContext(CajetaParser::PrimitiveTypeContext* ctx, CajetaModulePtr module);

        static CajetaTypePtr fromContext(CajetaParser::TypeTypeOrVoidContext* ctx, CajetaModulePtr module);

        static CajetaTypePtr fromContext(CajetaParser::TypeTypeContext* ctx, CajetaModulePtr module);

        // The resolution itself. `fromContext` above is a thin wrapper that also
        // records the resolved type as an xref reference edge (ide-symbol-index
        // 2.1.5), so every type name in the language is indexed at one point rather
        // than at each of its dozens of syntactic homes.
        //
        // The resolver's own recursive calls (a type argument, a function type's
        // parameter and return slots) go back through the WRAPPER, which is what we
        // want: in `ArrayList<Point>` both `ArrayList` and `Point` are names a
        // developer Ctrl-clicks, and each records at its own token.
        static CajetaTypePtr fromContextImpl(CajetaParser::TypeTypeContext* ctx, CajetaModulePtr module);

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

        // Mark / query a prescan-noted VIEW declaration. Read by
        // fromContext's placeholder synthesis so a forward reference to a
        // view gets a CajetaView placeholder (view classification and
        // member lookup dynamic_cast the type), not a class shell.
        static void markArchiveView(const string& canonical);
        static bool isArchiveView(const string& canonical);

        // Mark a previously-registered archive entry as an @ValueType
        // class. Read by fromContext's placeholder-synthesis path so a
        // cross-file value-type-typed declaration (`Vec2 a;`) is born
        // carrying VALUE_TYPE_FLAG | BY_VALUE_FLAG — eliminating the
        // stale-instance gap where the canonical CajetaClass gets the
        // flag in generatePrototype but earlier placeholders do not.
        // Mirrors markArchiveEnum; called by the prescan visitor's
        // visitClassDeclaration when the class is annotated @ValueType.
        static void markArchiveValueType(const string& canonical);
        static bool isArchiveValueType(const string& canonical);

        // Mark a previously-registered archive entry as an INTERFACE
        // declaration. Read by fromContext's placeholder-synthesis path
        // so a cross-file field/param/local declared at a forward-
        // referenced interface type (e.g. `ByteChannel stream;` in
        // AsyncReader before ByteChannel.cajeta is parsed) is born as a
        // FAT 24-byte interface pointer `{ ptr data, ptr vtable, i64 kind }`
        // — not a thin 8-byte class pointer that silently drops interface
        // dispatch at codegen. Called by the prescan visitor's
        // visitInterfaceDeclaration after registerArchive(canonical,
        // shortName). See cajeta-interface-arg-field-offset-bug.
        static void markArchiveInterface(const string& canonical);
        static bool isArchiveInterface(const string& canonical);

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

        // Test stdlib-reuse support. captureBaseline() snapshots every global
        // type container (canonicalMap, typeMap, archives, …) right after the
        // pristine stdlib is built; restoreBaseline() assigns those snapshots
        // back, wiping all user-added types AND any user-triggered stdlib
        // template instantiations in one shot, so each test starts from the
        // exact post-stdlib state without re-parsing. No-ops outside the
        // reuse path (production never calls them).
        static void captureBaseline();
        static void restoreBaseline();
        // lint-server sibling-context reuse (spec §4): a SECOND baseline slot
        // holding "stdlib + the sibling sweep", captured after
        // registerLintContext and restored (independently of the pristine
        // stdlib baseline) on a warm request so it skips the sweep.
        // invalidate clears it so the next request resweeps. No-ops until a
        // context is captured (production one-shot never captures one).
        static void captureContextBaseline();
        static void restoreContextBaseline();
        static void invalidateContextBaseline();
        // Test stdlib-reuse support: free the shared-context LLVM struct NAMES of
        // the transient user types a THROWING compile left behind (a test whose
        // compile threw never reached its normal end-of-compile struct-name
        // release), so a later same-named test can't pick up a stale layout via
        // StructType::getTypeByName. Preserves stdlib-resident (reusable)
        // instantiations. No-op outside the reuse path.
        static void releaseThrownTransientStructNames();


        static llvm::StructType* getOrCreateLlvmType(llvm::LLVMContext* ctx, string name, vector<llvm::Type*> properties);
        static llvm::StructType* getOrCreateLlvmType(llvm::LLVMContext* ctx, string name);

        // U6.4.2 — like getOrCreateLlvmType(ctx, name) but WITHOUT the
        // canonicalMap registration side-effect. The frozen-stdlib per-thread
        // struct rebuild needs only the (opaque) named StructType in the thread's
        // context; it must NOT re-register a plain CajetaType over the shared
        // class/view entry in the thread's registry. Returns the existing struct
        // by name in `ctx` if present, else creates a fresh opaque one.
        static llvm::StructType* getOrCreateLlvmStructNoRegister(llvm::LLVMContext* ctx, const string& name);

        static CajetaTypePtr create(QualifiedNamePtr qName) {
            CajetaTypePtr result = make_shared<CajetaType>(qName);
            // Guard: a qName-only CajetaType has no llvmType yet, and
            // TypeKey(nullptr) dereferences it — only index by type when present.
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
        // live in docs/TemplateWildcard.md and todo.md.
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

        // The error/poison type (diagnostic-engine-spec §3): a singleton returned
        // when resolution fails, so semantic analysis continues instead of
        // throwing. Not registered in the type map (unfindable by name). Codegen
        // is gated on the diagnostic engine having no errors, so it is never
        // lowered. `isError()` is true only for this sentinel.
        static CajetaTypePtr error();
        bool isError() const;

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
        // (docs/TemplateWildcard.md § 3 Capture identity). When
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

        // Where this type is declared. Only meaningful for types that came from a
        // parsed declaration; see the field comments.
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