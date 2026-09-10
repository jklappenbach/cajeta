//
// Created by James Klappenbach on 10/2/22.
//

#include "CajetaType.h"
#include "../field/Field.h"
#include <cstdlib>
#include <optional>
#include <set>
#include <unordered_map>
#include "../compile/CajetaModule.h"
#include "CajetaArray.h"
#include "CajetaCapture.h"
#include "CajetaClass.h"
#include "CajetaView.h"
#include "CajetaTask.h"
#include "CajetaConstantType.h"
#include "CajetaVector.h"
#include "CajetaMatrix.h"
#include "CajetaQuaternion.h"
#include "CajetaFunctionType.h"
#include "../compile/CompilationContext.h"
#include "../error/InvalidOperandException.h"
#include "../error/Exception.h"
#include "cajeta/xref/XrefIndex.h"

namespace cajeta {

    // Rebuild a scalar (primitive) LLVM type in `ctx` from a prototype built in
    // another context: TypeID and integer bit-width are context-independent, so a
    // frozen primitive can clone its shape per thread. Null if `proto` is not scalar.
    static llvm::Type* cloneScalarLlvmType(llvm::Type* proto, llvm::LLVMContext& ctx) {
        if (!proto) return nullptr;
        switch (proto->getTypeID()) {
            case llvm::Type::HalfTyID:    return llvm::Type::getHalfTy(ctx);
            case llvm::Type::BFloatTyID:  return llvm::Type::getBFloatTy(ctx);
            case llvm::Type::FloatTyID:   return llvm::Type::getFloatTy(ctx);
            case llvm::Type::DoubleTyID:  return llvm::Type::getDoubleTy(ctx);
            case llvm::Type::FP128TyID:   return llvm::Type::getFP128Ty(ctx);
            case llvm::Type::VoidTyID:    return llvm::Type::getVoidTy(ctx);
            case llvm::Type::IntegerTyID: return llvm::Type::getIntNTy(ctx, proto->getIntegerBitWidth());
            case llvm::Type::PointerTyID: return llvm::PointerType::get(ctx, 0);
            default:                      return nullptr;
        }
    }

    #define CAJETA_NATIVE_PACKAGE ""
    #define NATIVE_TYPE_ENTRY(typeName, llvmType, typeFlags) CajetaType::create(QualifiedName::getOrInsert(typeName, CAJETA_NATIVE_PACKAGE), llvmType, typeFlags);

    // Resolve a built-in Vector/Matrix dimension argument to its constant value:
    // an integer literal, or a bound const generic parameter resolved through the
    // active substitution map. Throws `errId` (naming `what`) if it is neither.
    static int64_t vectorLengthArg(CajetaParser::TypeArgumentContext* arg,
                                   CajetaModulePtr module,
                                   const char* errId, const char* what) {
        if (arg->integerLiteral() != nullptr) {
            return CajetaConstantType::parseLiteral(arg->integerLiteral());
        }
        CajetaTypePtr resolved;
        if (arg->typeType()) {
            resolved = CajetaType::fromContext(arg->typeType(), module);
        }
        if (auto c = std::dynamic_pointer_cast<CajetaConstantType>(resolved)) {
            return c->getValue();
        }
        throw Exception(
            std::string(what) + " must be a positive integer literal constant "
            "or a bound const generic parameter",
            errId);
    }

    // Per-thread: concurrent compiles must not share type state.
    thread_local map<string, CajetaTypePtr> CajetaType::canonicalMap;
    thread_local map<string, map<string, int32_t>> CajetaType::enumConstants;
    thread_local map<string, map<string, EnumConstantPos>>
        CajetaType::enumConstantPositions;
    thread_local map<TypeKey, CajetaTypePtr> CajetaType::typeMap;
    thread_local map<llvm::Type::TypeID, CajetaTypePtr> CajetaType::llvmTypeIdMap;

    // Per-thread LLVM-type bindings for FROZEN (shared stdlib) CajetaType objects,
    // keyed by the shared object: a frozen object's inline `llvmType` belongs to the
    // prime context, so each thread resolves its own binding here.
    static std::unordered_map<const CajetaType*, llvm::Type*>& frozenTypeBindings() {
        static thread_local std::unordered_map<const CajetaType*, llvm::Type*> tbl;
        return tbl;
    }

    llvm::Type* CajetaType::rawLlvmType() const {
        if (frozen) {
            auto& tbl = frozenTypeBindings();
            auto it = tbl.find(this);
            return it != tbl.end() ? it->second : nullptr;
        }
        return llvmType;
    }

    llvm::Type* CajetaType::getLlvmType() {
        if (frozen && (typeFlags & PRIMITIVE_FLAG) && rawLlvmType() == nullptr && llvmType) {
            if (llvm::LLVMContext* ctx = currentLlvmContext()) {
                if (llvm::Type* t = cloneScalarLlvmType(llvmType, *ctx)) setLlvmType(t);
            }
        }
        return rawLlvmType();
    }

    void CajetaType::setLlvmType(llvm::Type* t) {
        if (frozen) { frozenTypeBindings()[this] = t; return; }
        llvmType = t;
    }
    // Prescan archives (see CajetaType.h), cleared by resetGlobals: canonical and
    // short name -> canonical, plus side sets recording which archived names are
    // enums, views, @ValueType classes or interfaces. Read by placeholder synthesis.
    static thread_local map<string, string> g_archive;
    static thread_local set<string> g_enumArchive;
    static thread_local set<string> g_viewArchive;
    static thread_local set<string> g_valueTypeArchive;
    static thread_local set<string> g_interfaceArchive;
    // Per-class template metadata from the prescan: lets a placeholder answer
    // isTemplate() and instantiate `T<args>` before its real declaration is visited.
    struct ArchiveTemplateMeta {
        vector<TypeParameter> typeParameters;
        string templateSource;
    };
    static thread_local map<string, ArchiveTemplateMeta> g_archiveTemplateMeta;
    // canonical -> declaring source file, recorded for on-disk user sources only.
    static thread_local map<string, string> g_archiveSourcePaths;
    // Null means "fall back to the CAJETA_WILDCARDS env var" (the production path).
    static std::optional<bool> g_wildcardsTestOverride;
    // Wildcard info keyed by sentinel canonical (`?`, `? extends X`, `? super X`).
    struct WildcardInfoEntry {
        CajetaType::WildcardKind kind;
        CajetaTypePtr bound;
    };
    static thread_local map<string, WildcardInfoEntry> g_wildcardInfo;


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

    // Lexicographic (typeId, then typeCode). Must be a strict weak ordering: less
    // than that corrupts the std::map<TypeKey, ...> registries that of() reads.
    bool operator<(const TypeKey& a, const TypeKey& b) {
        if (a.typeId != b.typeId) return a.typeId < b.typeId;
        return a.typeCode < b.typeCode;
    }

    void CajetaType::resetGlobals() {
        // Drop first: a previous compile's abandoned deferred instantiations point
        // into a dead object graph and would drain against half-built registries.
        CajetaClass::resetDeferredInstantiationState();
        canonicalMap.clear();
        typeMap.clear();
        llvmTypeIdMap.clear();
        enumConstants.clear();
        enumConstantPositions.clear();
        g_archive.clear();
        g_enumArchive.clear();
        g_viewArchive.clear();
        g_valueTypeArchive.clear();
        g_interfaceArchive.clear();
        g_archiveTemplateMeta.clear();
        g_archiveSourcePaths.clear();
        g_wildcardInfo.clear();
        // The test override survives resetGlobals on purpose.
    }

    namespace {
        // Snapshot of every global type container, taken once after the pristine
        // stdlib is built (StdlibCache::prime) and reassigned before each reusing
        // test. `valid` guards the production case where none was ever captured.
        struct TypeGlobalsBaseline {
            bool valid = false;
            map<string, CajetaTypePtr> canonicalMap;
            map<string, map<string, int32_t>> enumConstants;
            map<TypeKey, CajetaTypePtr> typeMap;
            map<llvm::Type::TypeID, CajetaTypePtr> llvmTypeIdMap;
            map<string, string> g_archive;
            set<string> g_enumArchive;
            set<string> g_viewArchive;
            set<string> g_valueTypeArchive;
            set<string> g_interfaceArchive;
            map<string, ArchiveTemplateMeta> g_archiveTemplateMeta;
            map<string, string> g_archiveSourcePaths;
            map<string, WildcardInfoEntry> g_wildcardInfo;
        };
        TypeGlobalsBaseline g_typeBaseline;
        // A second, independent slot for the lint-server sibling context ("stdlib +
        // the sibling sweep"), so a warm request restores it instead of resweeping.
        TypeGlobalsBaseline g_typeContextBaseline;

        // Copy every global type container into `b` and mark it valid.
        void captureTypeBaselineInto(TypeGlobalsBaseline& b,
                                     const map<string, CajetaTypePtr>& canonicalMap,
                                     const map<string, map<string, int32_t>>& enumConstants,
                                     const map<TypeKey, CajetaTypePtr>& typeMap,
                                     const map<llvm::Type::TypeID, CajetaTypePtr>& llvmTypeIdMap) {
            b.canonicalMap = canonicalMap;
            b.enumConstants = enumConstants;
            b.typeMap = typeMap;
            b.llvmTypeIdMap = llvmTypeIdMap;
            b.g_archive = g_archive;
            b.g_enumArchive = g_enumArchive;
            b.g_viewArchive = g_viewArchive;
            b.g_valueTypeArchive = g_valueTypeArchive;
            b.g_interfaceArchive = g_interfaceArchive;
            b.g_archiveTemplateMeta = g_archiveTemplateMeta;
            b.g_archiveSourcePaths = g_archiveSourcePaths;
            b.g_wildcardInfo = g_wildcardInfo;
            b.valid = true;
        }
    }

    void CajetaType::captureBaseline() {
        captureTypeBaselineInto(g_typeBaseline, canonicalMap, enumConstants,
                                typeMap, llvmTypeIdMap);
    }

    void CajetaType::captureContextBaseline() {
        captureTypeBaselineInto(g_typeContextBaseline, canonicalMap, enumConstants,
                                typeMap, llvmTypeIdMap);
    }

    void CajetaType::restoreContextBaseline() {
        if (!g_typeContextBaseline.valid) return;
        canonicalMap = g_typeContextBaseline.canonicalMap;
        enumConstants = g_typeContextBaseline.enumConstants;
        typeMap = g_typeContextBaseline.typeMap;
        llvmTypeIdMap = g_typeContextBaseline.llvmTypeIdMap;
        g_archive = g_typeContextBaseline.g_archive;
        g_enumArchive = g_typeContextBaseline.g_enumArchive;
        g_viewArchive = g_typeContextBaseline.g_viewArchive;
        g_valueTypeArchive = g_typeContextBaseline.g_valueTypeArchive;
        g_interfaceArchive = g_typeContextBaseline.g_interfaceArchive;
        g_archiveTemplateMeta = g_typeContextBaseline.g_archiveTemplateMeta;
        g_archiveSourcePaths = g_typeContextBaseline.g_archiveSourcePaths;
        g_wildcardInfo = g_typeContextBaseline.g_wildcardInfo;
    }

    void CajetaType::invalidateContextBaseline() {
        g_typeContextBaseline = TypeGlobalsBaseline{};
    }

    void CajetaType::releaseThrownTransientStructNames() {
        if (!g_typeBaseline.valid) return;
        // Release by NAME, walking canonicalMap (the authoritative creation record,
        // which also catches structs floating free of any module), but PRESERVE
        // stdlib-resident ones: the stdlib module reuses them by name across tests.
        std::set<std::string> stdlibResident;
        if (auto stdlib = CajetaModule::getStdlibModule()) {
            if (auto* lm = stdlib->getLlvmModule()) {
                for (auto* st : lm->getIdentifiedStructTypes())
                    if (st->hasName()) stdlibResident.insert(st->getName().str());
            }
        }
        for (auto& [name, type] : canonicalMap) {
            if (!type) continue;
            llvm::Type* lt = type->getLlvmType();
            if (lt && lt->isStructTy()) {
                auto* st = llvm::cast<llvm::StructType>(lt);
                if (st->hasName() && !stdlibResident.count(st->getName().str()))
                    st->setName("");
            }
        }
    }

    void CajetaType::restoreBaseline() {
        if (!g_typeBaseline.valid) return;
        CajetaClass::resetDeferredInstantiationState();
        canonicalMap = g_typeBaseline.canonicalMap;
        enumConstants = g_typeBaseline.enumConstants;
        typeMap = g_typeBaseline.typeMap;
        llvmTypeIdMap = g_typeBaseline.llvmTypeIdMap;
        g_archive = g_typeBaseline.g_archive;
        g_enumArchive = g_typeBaseline.g_enumArchive;
        g_viewArchive = g_typeBaseline.g_viewArchive;
        g_valueTypeArchive = g_typeBaseline.g_valueTypeArchive;
        g_interfaceArchive = g_typeBaseline.g_interfaceArchive;
        g_archiveTemplateMeta = g_typeBaseline.g_archiveTemplateMeta;
        g_archiveSourcePaths = g_typeBaseline.g_archiveSourcePaths;
        g_wildcardInfo = g_typeBaseline.g_wildcardInfo;
    }

    map<string, string>& CajetaType::getArchive() { return g_archive; }

    void CajetaType::markArchiveEnum(const string& canonical) {
        g_enumArchive.insert(canonical);
    }

    bool CajetaType::isArchiveEnum(const string& canonical) {
        return g_enumArchive.count(canonical) > 0;
    }

    void CajetaType::markArchiveView(const string& canonical) {
        g_viewArchive.insert(canonical);
    }

    bool CajetaType::isArchiveView(const string& canonical) {
        return g_viewArchive.count(canonical) > 0;
    }

    void CajetaType::markArchiveValueType(const string& canonical) {
        g_valueTypeArchive.insert(canonical);
    }

    bool CajetaType::isArchiveValueType(const string& canonical) {
        return g_valueTypeArchive.count(canonical) > 0;
    }

    void CajetaType::registerArchiveSourcePath(const string& canonical,
                                               const string& sourcePath) {
        g_archiveSourcePaths.emplace(canonical, sourcePath);
    }

    string CajetaType::lookupArchiveSourcePath(const string& canonical) {
        auto it = g_archiveSourcePaths.find(canonical);
        return it == g_archiveSourcePaths.end() ? string() : it->second;
    }

    void CajetaType::markArchiveInterface(const string& canonical) {
        g_interfaceArchive.insert(canonical);
    }

    bool CajetaType::isArchiveInterface(const string& canonical) {
        return g_interfaceArchive.count(canonical) > 0;
    }

    void CajetaType::registerArchiveTemplate(const string& canonical,
                                              const vector<TypeParameter>& typeParameters,
                                              const string& templateSource) {
        if (canonical.empty() || typeParameters.empty()) return;
        auto& meta = g_archiveTemplateMeta[canonical];
        meta.typeParameters = typeParameters;
        meta.templateSource = templateSource;
    }

    const vector<TypeParameter>* CajetaType::lookupArchiveTemplateParameters(
            const string& canonical) {
        auto it = g_archiveTemplateMeta.find(canonical);
        if (it == g_archiveTemplateMeta.end()) return nullptr;
        return &it->second.typeParameters;
    }

    const string* CajetaType::lookupArchiveTemplateSource(
            const string& canonical) {
        auto it = g_archiveTemplateMeta.find(canonical);
        if (it == g_archiveTemplateMeta.end()) return nullptr;
        return &it->second.templateSource;
    }

    bool CajetaType::wildcardsEnabled() {
        if (g_wildcardsTestOverride.has_value()) {
            return *g_wildcardsTestOverride;
        }
        const char* v = std::getenv("CAJETA_WILDCARDS");
        if (v == nullptr) return true;
        return v[0] != '\0' && v[0] != '0';
    }

    void CajetaType::setWildcardsEnabledForTest(bool enabled) {
        g_wildcardsTestOverride = enabled;
    }

    void CajetaType::clearWildcardsTestOverride() {
        g_wildcardsTestOverride.reset();
    }

    CajetaTypePtr CajetaType::wildcardSentinel() {
        auto it = canonicalMap.find("?");
        if (it == canonicalMap.end()) return nullptr;
        return it->second;
    }

    // Lazily create (or reuse) the sentinel for a bounded wildcard, keyed by the
    // canonical "? <kindWord> <bound>". Shares the unbounded sentinel's opaque
    // pointer type; the bound is recorded in g_wildcardInfo for wildcardBound().
    static CajetaTypePtr makeBoundedWildcardSentinel(
            CajetaType::WildcardKind kind,
            CajetaTypePtr bound,
            const string& kindWord) {
        if (!bound || !bound->getQName()) return nullptr;
        string canonical = "? " + kindWord + " "
            + bound->getQName()->toCanonical();
        auto& cmap = CajetaType::getCanonicalMap();
        auto it = cmap.find(canonical);
        if (it != cmap.end()) return it->second;
        auto unbounded = CajetaType::wildcardSentinel();
        if (!unbounded) return nullptr;
        CajetaTypePtr sentinel = CajetaType::create(
            QualifiedName::getOrInsert(canonical, ""),
            unbounded->getLlvmType(),
            STRUCT_FLAG,
            /*shareLlvmType=*/false);
        g_wildcardInfo[canonical] = {kind, bound};
        return sentinel;
    }

    CajetaTypePtr CajetaType::wildcardSentinelExtends(CajetaTypePtr bound) {
        return makeBoundedWildcardSentinel(
            WildcardKind::Extends, bound, "extends");
    }

    CajetaTypePtr CajetaType::wildcardSentinelSuper(CajetaTypePtr bound) {
        return makeBoundedWildcardSentinel(
            WildcardKind::Super, bound, "super");
    }

    bool CajetaType::isWildcard() const {
        if (!qName) return false;
        auto it = g_wildcardInfo.find(qName->toCanonical());
        return it != g_wildcardInfo.end()
            && it->second.kind != WildcardKind::None;
    }

    CajetaTypePtr CajetaType::error() {
        // Built directly, not via create(), so it never lands in canonicalMap.
        static CajetaTypePtr sentinel =
            std::make_shared<CajetaType>(QualifiedName::getOrCreate("<error>"));
        return sentinel;
    }

    bool CajetaType::isError() const {
        return this == error().get();
    }

    CajetaType::WildcardKind CajetaType::wildcardKind() const {
        if (!qName) return WildcardKind::None;
        auto it = g_wildcardInfo.find(qName->toCanonical());
        return it == g_wildcardInfo.end()
            ? WildcardKind::None : it->second.kind;
    }

    CajetaTypePtr CajetaType::wildcardBound() const {
        if (!qName) return nullptr;
        auto it = g_wildcardInfo.find(qName->toCanonical());
        return it == g_wildcardInfo.end() ? nullptr : it->second.bound;
    }

    void CajetaType::registerWildcardInfo(const string& canonical,
                                           WildcardKind kind,
                                           CajetaTypePtr bound) {
        g_wildcardInfo[canonical] = {kind, bound};
    }

    CajetaTypePtr CajetaType::captureProject(CajetaTypePtr t) {
        if (!t) return t;
        if (auto cap = dynamic_pointer_cast<CajetaCapture>(t)) {
            auto upper = cap->getUpperBound();
            return upper ? upper : t;
        }
        if (t->wildcardKind() != WildcardKind::Extends) return t;
        auto bound = t->wildcardBound();
        return bound ? bound : t;
    }

    void CajetaType::registerArchive(const string& canonical,
                                      const string& shortName) {
        // Both keys point at the canonical so a bare short name can be promoted to
        // the right qualified placeholder. First write wins for either key.
        if (!canonical.empty() && g_archive.count(canonical) == 0) {
            g_archive[canonical] = canonical;
        }
        if (!shortName.empty() && g_archive.count(shortName) == 0) {
            g_archive[shortName] = canonical;
        }
    }

    // Register every built-in primitive (and the wildcard sentinel) in `ctx`.
    // Called once per Compiler, after resetGlobals.
    void CajetaType::init(llvm::LLVMContext& ctx) {
        NATIVE_TYPE_ENTRY("void", llvm::Type::getVoidTy(ctx), VOID_TYPE_ID);
        NATIVE_TYPE_ENTRY("boolean", llvm::Type::getInt1Ty(ctx), BOOLEAN_TYPE_ID);
        // `char` is the 32-bit Unicode codepoint type (Go's `rune`), NOT the 8-bit
        // byte -- that is `int8`/`uint8`. `uchar` stays as a deprecated alias for
        // uint8, with shareLlvmType=false so the reverse i8 lookup lands on uint8.
        NATIVE_TYPE_ENTRY("uchar", llvm::Type::getInt8Ty(ctx), UINT8_TYPE_ID);
        NATIVE_TYPE_ENTRY("char", llvm::Type::getInt32Ty(ctx), INT32_TYPE_ID);
        CajetaType::create(QualifiedName::getOrInsert("uint8", CAJETA_NATIVE_PACKAGE),
            llvm::Type::getInt8Ty(ctx), UINT8_TYPE_ID, /*shareLlvmType=*/false);
        CajetaType::create(QualifiedName::getOrInsert("int8", CAJETA_NATIVE_PACKAGE),
            llvm::Type::getInt8Ty(ctx), INT8_TYPE_ID, /*shareLlvmType=*/false);
        NATIVE_TYPE_ENTRY("uint16", llvm::Type::getInt16Ty(ctx), UINT16_TYPE_ID);
        NATIVE_TYPE_ENTRY("int16", llvm::Type::getInt16Ty(ctx), INT16_TYPE_ID);
        NATIVE_TYPE_ENTRY("uint32", llvm::Type::getInt32Ty(ctx), UINT32_TYPE_ID);
        NATIVE_TYPE_ENTRY("int32", llvm::Type::getInt32Ty(ctx), INT32_TYPE_ID);
        NATIVE_TYPE_ENTRY("uint64", llvm::Type::getInt64Ty(ctx), UINT64_TYPE_ID);
        NATIVE_TYPE_ENTRY("int64", llvm::Type::getInt64Ty(ctx), INT64_TYPE_ID);
        NATIVE_TYPE_ENTRY("uint128", llvm::Type::getInt128Ty(ctx), UINT128_TYPE_ID);
        NATIVE_TYPE_ENTRY("int128", llvm::Type::getInt128Ty(ctx), INT128_TYPE_ID);
        // Sub-byte and 8-bit floats from the OCP Microscaling spec: LLVM has no
        // IR-level Type* for them, so they are stored as iN. shareLlvmType=false
        // keeps the iN registration from overwriting the int{4,6,8} entries.
        #define FP_OPAQUE_ENTRY(typeName, bits, typeFlags) \
            CajetaType::create(QualifiedName::getOrInsert(typeName, CAJETA_NATIVE_PACKAGE), \
                llvm::IntegerType::get(ctx, bits), typeFlags, /*shareLlvmType=*/false);
        FP_OPAQUE_ENTRY("float4e2m1",     4, FLOAT4E2M1_TYPE_ID);
        FP_OPAQUE_ENTRY("float6e2m3",     6, FLOAT6E2M3_TYPE_ID);
        FP_OPAQUE_ENTRY("float6e3m2",     6, FLOAT6E3M2_TYPE_ID);
        FP_OPAQUE_ENTRY("float8e4m3",     8, FLOAT8E4M3_TYPE_ID);
        FP_OPAQUE_ENTRY("float8e5m2",     8, FLOAT8E5M2_TYPE_ID);
        FP_OPAQUE_ENTRY("float8e4m3fnuz", 8, FLOAT8E4M3FNUZ_TYPE_ID);
        FP_OPAQUE_ENTRY("float8e5m2fnuz", 8, FLOAT8E5M2FNUZ_TYPE_ID);
        #undef FP_OPAQUE_ENTRY
        // float16 is IEEE-754 binary16 (LLVM `half`); bfloat16 is a distinct format
        // -- same width, float32's exponent. Don't conflate the two.
        NATIVE_TYPE_ENTRY("float16", llvm::Type::getHalfTy(ctx), FLOAT16_TYPE_ID);
        NATIVE_TYPE_ENTRY("bfloat16", llvm::Type::getBFloatTy(ctx), BFLOAT16_TYPE_ID);
        NATIVE_TYPE_ENTRY("float32", llvm::Type::getFloatTy(ctx), FLOAT32_TYPE_ID);
        NATIVE_TYPE_ENTRY("float64", llvm::Type::getDoubleTy(ctx), FLOAT64_TYPE_ID);
        NATIVE_TYPE_ENTRY("float128", llvm::Type::getFP128Ty(ctx), FLOAT128_TYPE_ID);
        NATIVE_TYPE_ENTRY("pointer", llvm::PointerType::get(ctx, 0), POINTER_TYPE_ID);
        // The `?` sentinel is registered regardless of the feature flag so
        // wildcardSentinel() is never null; shareLlvmType=false keeps its opaque
        // pointer out of the canonical `pointer` entries in typeMap/llvmTypeIdMap.
        CajetaType::create(
            QualifiedName::getOrInsert("?", CAJETA_NATIVE_PACKAGE),
            llvm::PointerType::get(ctx, 0),
            STRUCT_FLAG,
            /*shareLlvmType=*/false);
        g_wildcardInfo["?"] = {CajetaType::WildcardKind::Unbounded, nullptr};
    }

    llvm::ConstantInt* CajetaType::getTypeAllocSize(CajetaModulePtr module) {
        const llvm::DataLayout& dataLayout = module->getLlvmModule()->getDataLayout();
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*module->getLlvmContext()),
            dataLayout.getTypeAllocSize(rawLlvmType()));
    }

    // The generic token this type keys on for dispatch: "number", "void",
    // "function" or "pointer" for primitives, the canonical name otherwise.
    string CajetaType::toGeneric() {
        if (typeFlags & PRIMITIVE_FLAG) {
            // getLlvmType(), not the llvmType field: CajetaVector / CajetaMatrix
            // build theirs lazily and start null.
            switch (getLlvmType()->getTypeID()) {
                case llvm::Type::HalfTyID:
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
                case llvm::Type::FixedVectorTyID:
                case llvm::Type::ScalableVectorTyID:
                    return toCanonical();
                default:
                    return "unknown";
            }
        } else {
            return canonical;
        }
    }

    map<string, CajetaTypePtr>& CajetaType::getCanonicalMap() { return canonicalMap; }

    bool CajetaType::isValueType() const {
        if (typeFlags & VALUE_TYPE_FLAG) {
            return true;
        }
        if (!qName) {
            return false;
        }
        auto it = canonicalMap.find(qName->toCanonical());
        if (it != canonicalMap.end() && it->second && it->second.get() != this) {
            return (it->second->getTypeFlags() & VALUE_TYPE_FLAG) != 0;
        }
        return false;
    }

    CajetaTypePtr CajetaType::of(string typeName) {
        QualifiedNamePtr qName = QualifiedName::getOrCreate(typeName);
        auto it = canonicalMap.find(qName->toCanonical());
        return it == canonicalMap.end() ? nullptr : it->second;
    }

    CajetaTypePtr CajetaType::find(const string& typeName) {
        auto it = canonicalMap.find(typeName);
        if (it != canonicalMap.end()) return it->second;
        QualifiedNamePtr qName = QualifiedName::getOrCreate(typeName);
        it = canonicalMap.find(qName->toCanonical());
        return it == canonicalMap.end() ? nullptr : it->second;
    }

    // The package that scopes a bare name in the code being processed: the current
    // method's declaring class, else the top of the structure stack, else the
    // module's own package (the module qName is stale by codegen time).
    static string scopePackageOf(CajetaModulePtr module) {
        if (!module) return {};
        if (auto m = module->getCurrentMethod()) {
            if (m->getParent() && m->getParent()->getQName()) {
                const string& p =
                    m->getParent()->getQName()->getPackageName();
                if (!p.empty()) return p;
            }
        }
        if (!module->getStructureStack().empty()) {
            auto& top = module->getStructureStack().back();
            if (top && top->getQName()) {
                const string& p = top->getQName()->getPackageName();
                if (!p.empty()) return p;
            }
        }
        if (module->getQName()) return module->getQName()->getPackageName();
        return {};
    }

    CajetaTypePtr CajetaType::ofScoped(const string& shortName,
                                       CajetaModulePtr module) {
        {
            string ownPkg = scopePackageOf(module);
            if (!ownPkg.empty()) {
                auto ownIt = canonicalMap.find(ownPkg + "." + shortName);
                if (ownIt != canonicalMap.end() && ownIt->second) {
                    return ownIt->second;
                }
            }
        }
        if (module) {
            auto& imports = module->getImports();
            auto importIt = imports.find(shortName);
            if (importIt != imports.end() && !importIt->second.empty()) {
                auto canonIt = canonicalMap.find(
                    importIt->second.begin()->second->toCanonical());
                if (canonIt != canonicalMap.end() && canonIt->second) {
                    return canonIt->second;
                }
                return nullptr;
            }
        }
        auto it = canonicalMap.find(shortName);
        return it != canonicalMap.end() ? it->second : nullptr;
    }

    std::string CajetaType::canonicalNameScoped(const string& shortName,
                                                CajetaModulePtr module) {
        string ownPkg = scopePackageOf(module);
        if (!ownPkg.empty()) {
            string canon = ownPkg + "." + shortName;
            if (canonicalMap.count(canon) || g_archive.count(canon)) return canon;
        }
        if (module) {
            auto& imports = module->getImports();
            auto it = imports.find(shortName);
            if (it != imports.end() && !it->second.empty()) {
                return it->second.begin()->second->toCanonical();
            }
        }
        auto a = g_archive.find(shortName);
        if (a != g_archive.end()) return a->second;
        return {};
    }

    CajetaTypePtr CajetaType::of(string typeName, string package) {
        QualifiedNamePtr qName = QualifiedName::getOrInsert(typeName, package);
        auto it = canonicalMap.find(qName->toCanonical());
        return it == canonicalMap.end() ? nullptr : it->second;
    }

    CajetaTypePtr CajetaType::of(QualifiedNamePtr qName) {
        auto it = canonicalMap.find(qName->toCanonical());
        return it == canonicalMap.end() ? nullptr : it->second;
    }

    CajetaTypePtr CajetaType::findTemplateByShortName(const string& shortName) {
        for (auto& kv : canonicalMap) {
            auto cand = std::dynamic_pointer_cast<CajetaClass>(kv.second);
            if (cand && cand->isTemplate() && cand->getQName()
                    && cand->getQName()->getTypeName() == shortName) {
                return std::static_pointer_cast<CajetaType>(cand);
            }
        }
        return nullptr;
    }

    CajetaTypePtr CajetaType::fromContext(CajetaParser::PrimitiveTypeContext* ctx, CajetaModulePtr module) {
        QualifiedNamePtr qName = QualifiedName::getOrInsert(ctx->getText(), "code");
        auto it = canonicalMap.find(qName->toCanonical());
        return it == canonicalMap.end() ? nullptr : it->second;
    }

    cajeta::CajetaTypePtr cajeta::CajetaType::fromContext(CajetaParser::TypeTypeOrVoidContext* ctx, CajetaModulePtr module) {
        CajetaTypePtr type = nullptr;
        if (ctx != nullptr) {
            if (ctx->VOID() != nullptr) {
                QualifiedNamePtr qName = QualifiedName::getOrCreate(ctx->getText());
                auto it = canonicalMap.find(qName->toCanonical());
                type = it == canonicalMap.end() ? nullptr : it->second;
            } else {
                type = fromContext(ctx->typeType(), module);
            }
        }
        return type;
    }

    // Parse a `T[N]` bracket size literal into a fixed inline length: the value of a
    // plain non-negative decimal or `0x` literal (underscores allowed), -1 for
    // anything non-constant -- those stay heap references.
    static int32_t parseConstantArrayLength(const std::string& text) {
        std::string s;
        for (char c : text) {
            if (c != '_') s += c;
        }
        if (s.empty()) return -1;
        try {
            size_t pos = 0;
            int base = (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10;
            long v = std::stol(s, &pos, base);
            if (pos != s.size()) return -1;
            if (v < 0 || v > 0x7fffffff) return -1;
            return static_cast<int32_t>(v);
        } catch (...) {
            return -1;
        }
    }

    CajetaTypePtr CajetaType::resolveNamed(QualifiedNamePtr qName,
                                           CajetaModulePtr module) {
        if (!module) {
            module = CajetaModule::getActiveModule();
        }
        CajetaTypePtr type;
        if (module) {
            CajetaTypePtr substituted = module->lookupTypeParameter(qName->getTypeName());
            if (substituted) {
                type = substituted;
            }
        }
        if (!type) {
            auto it = canonicalMap.find(qName->toCanonical());
            // An annotation type must NOT satisfy a plain type reference: a bare
            // `Foo` canonicalizes to "code.Foo", which an @Foo annotation owns too.
            bool annHit = false;
            if (it != canonicalMap.end()) {
                auto kc = std::dynamic_pointer_cast<CajetaClass>(it->second);
                annHit = kc && kc->isAnnotation();
            }
            const bool bareName = qName->getPackageName().empty()
                                  || qName->getPackageName() == "code";
            std::string ownPkgCanonical;
            std::string importedCanonical;
            std::string scopePkg =
                bareName ? scopePackageOf(module) : std::string();
            if (bareName && !scopePkg.empty()) {
                ownPkgCanonical = scopePkg + "." + qName->getTypeName();
                auto ownIt = canonicalMap.find(ownPkgCanonical);
                if (ownIt != canonicalMap.end() && ownIt->second) {
                    auto oc = std::dynamic_pointer_cast<CajetaClass>(
                        ownIt->second);
                    if (!(oc && oc->isAnnotation())) {
                        type = ownIt->second;
                    }
                }
            }
            if (!type && bareName && module) {
                auto& imports = module->getImports();
                auto importIt = imports.find(qName->getTypeName());
                if (importIt != imports.end() && !importIt->second.empty()) {
                    auto& imported = importIt->second.begin()->second;
                    importedCanonical = imported->toCanonical();
                    auto canonIt = canonicalMap.find(importedCanonical);
                    if (canonIt != canonicalMap.end()) {
                        type = canonIt->second;
                    }
                }
            }
            // The global tiers are skipped when an import or the own-package archive
            // vouches for the name: the placeholder path below honors that scope.
            bool ownPkgArchived = false;
            if (!type && importedCanonical.empty()
                && !ownPkgCanonical.empty()) {
                ownPkgArchived =
                    getArchive().count(ownPkgCanonical) > 0;
            }
            if (!type && importedCanonical.empty() && !ownPkgArchived) {
                if (it != canonicalMap.end() && !annHit) {
                    type = it->second;
                } else {
                    // Fall back to the native ("") package: built-in aliases.
                    auto nativeIt = canonicalMap.find(qName->getTypeName());
                    if (nativeIt != canonicalMap.end()) {
                        type = nativeIt->second;
                    }
                }
            }
            if (!type && annHit) {
                type = it->second;
            }
            // Placeholder synthesis: the archive says the name IS declared in this
            // compilation unit, just not visited yet. The real visitClassDeclaration
            // later fills in this same shared_ptr.
            if (!type) {
                auto& archive = getArchive();
                std::string lookup = qName->toCanonical();
                auto archIt = archive.end();
                if (!importedCanonical.empty()) {
                    archIt = archive.find(importedCanonical);
                }
                if (archIt == archive.end() && !ownPkgCanonical.empty()) {
                    archIt = archive.find(ownPkgCanonical);
                }
                if (archIt == archive.end()) {
                    archIt = archive.find(lookup);
                }
                if (archIt == archive.end()) {
                    archIt = archive.find(qName->getTypeName());
                }
                if (archIt != archive.end()) {
                    const std::string& canonical = archIt->second;
                    auto dot = canonical.find_last_of('.');
                    std::string pkg = (dot == std::string::npos)
                        ? std::string()
                        : canonical.substr(0, dot);
                    std::string shortName = (dot == std::string::npos)
                        ? canonical
                        : canonical.substr(dot + 1);
                    QualifiedNamePtr phName =
                        QualifiedName::getOrInsert(shortName, pkg);
                    if (isArchiveEnum(canonical)) {
                        llvm::LLVMContext* ctx2 = module
                            ? module->getLlvmContext()
                            : nullptr;
                        if (ctx2) {
                            llvm::Type* i32Ty = llvm::Type::getInt32Ty(*ctx2);
                            type = CajetaType::create(phName, i32Ty,
                                INT_FLAG | SIGNED_FLAG | NUMBER_FLAG
                                    | PRIMITIVE_FLAG | BIT_32_FLAG
                                    | ENUM_FLAG,
                                /*shareLlvmType=*/false);
                            canonicalMap[shortName] = type;
                        }
                    } else if (isArchiveInterface(canonical)
                               && lookupArchiveTemplateParameters(canonical)
                                      == nullptr
                               && module) {
                        // A forward-referenced non-generic interface must be born
                        // FAT -- `{ ptr data, ptr vtable, i64 kind }`, 24 bytes -- or
                        // the owner reserves 8 and dispatch is dropped at codegen.
                        auto placeholder = std::make_shared<CajetaClass>(
                            module, phName,
                            std::list<QualifiedNamePtr>{},
                            std::list<QualifiedNamePtr>{});
                        placeholder->setPlaceholder(true);
                        placeholder->setIsInterface(true);
                        llvm::LLVMContext* ictx = module->getLlvmContext();
                        llvm::StructType* body =
                            CajetaType::getOrCreateLlvmType(ictx, canonical);
                        if (body->isOpaque()) {
                            llvm::Type* ptrTy =
                                llvm::PointerType::get(*ictx, 0);
                            llvm::Type* i64Ty =
                                llvm::Type::getInt64Ty(*ictx);
                            std::vector<llvm::Type*> members{
                                ptrTy, ptrTy, i64Ty };
                            body->setBody(
                                llvm::ArrayRef<llvm::Type*>(members), false);
                        }
                        placeholder->setLlvmType(body);
                        // Overwrite the plain CajetaType getOrCreateLlvmType just
                        // registered, so name lookups land the interface CajetaClass.
                        canonicalMap[canonical] = placeholder;
                        canonicalMap[shortName] = placeholder;
                        type = placeholder;
                    } else if (isArchiveView(canonical)) {
                        // The placeholder must BE a CajetaView: view classification
                        // and member lookup dynamic_cast the type.
                        auto placeholder = std::make_shared<CajetaView>(
                            module, phName);
                        placeholder->setPlaceholder(true);
                        canonicalMap[canonical] = placeholder;
                        canonicalMap[shortName] = placeholder;
                        type = placeholder;
                    } else {
                        auto placeholder = std::make_shared<CajetaClass>(
                            module, phName,
                            std::list<QualifiedNamePtr>{},
                            std::list<QualifiedNamePtr>{});
                        placeholder->setPlaceholder(true);
                        // Born-correct @ValueType, so any AST node that captures this
                        // placeholder sees the by-value storage axis directly.
                        if (isArchiveValueType(canonical)) {
                            placeholder->addTypeFlags(
                                VALUE_TYPE_FLAG | BY_VALUE_FLAG);
                        }
                        // A forward-referenced GENERIC interface takes this path;
                        // instantiateInternal keys on isInterface to re-parse it.
                        if (isArchiveInterface(canonical)) {
                            placeholder->setIsInterface(true);
                        }
                        if (const auto* archParams =
                                lookupArchiveTemplateParameters(canonical)) {
                            placeholder->setTypeParameters(*archParams);
                            if (const auto* archSrc =
                                    lookupArchiveTemplateSource(canonical)) {
                                placeholder->setTemplateSource(*archSrc);
                            }
                        }
                        // Leave llvmType null: the real generatePrototype's
                        // getOrCreateLlvmType must create the named StructType.
                        canonicalMap[canonical] = placeholder;
                        canonicalMap[shortName] = placeholder;
                        type = placeholder;
                    }
                }
            }
        }
        return type;
    }

    cajeta::CajetaTypePtr cajeta::CajetaType::fromContextImpl(CajetaParser::TypeTypeContext* ctx, CajetaModulePtr module) {
        if (!module) {
            module = CajetaModule::getActiveModule();
        }
        if (auto* fnt = ctx->functionType()) {
            std::vector<CajetaTypePtr> paramTypes;
            for (auto* p : fnt->typeType()) {
                paramTypes.push_back(fromContext(p, module));
            }
            CajetaTypePtr ret;
            // Function-type return ABI: `-> #R` is the pointer-return form
            // `R* (params)`; plain `-> R` is the sret form `void (ptr sret(R),
            // params)`. buildCanonical normalizes non-sret-eligible returns.
            bool returnsOwn = true;
            if (auto* rt = fnt->typeTypeOrVoid()) {
                if (rt->VOID()) {
                    QualifiedNamePtr voidQ = QualifiedName::getOrInsert(
                        "void", CAJETA_NATIVE_PACKAGE);
                    auto vIt = canonicalMap.find(voidQ->toCanonical());
                    if (vIt != canonicalMap.end()) ret = vIt->second;
                } else if (rt->typeType()) {
                    if (rt->CARET() != nullptr) {
                        throw Exception(
                            "`^` (view) returns are not supported on function "
                            "types: `(P) -> ^R` has no ABI form — function "
                            "values carry an ownership stance only. Use a "
                            "plain or `#R` return, or call the `^` method "
                            "directly. See "
                            "specs/stdlib-ownership-convention-spec.md §4.7.",
                            "CAJETA_ERROR_VIEW_REFERENCE_UNSUPPORTED");
                    }
                    ret = fromContext(rt->typeType(), module);
                    returnsOwn = (rt->REFERENCE() != nullptr);
                }
            }
            // A null return is legal here: an unresolved return-slot name stays null
            // rather than throwing, and buildCanonical gives it a "?" slot.
            std::string canon = CajetaFunctionType::buildCanonical(paramTypes, ret, returnsOwn);
            CajetaTypePtr fnAsType;
            auto it = canonicalMap.find(canon);
            if (it != canonicalMap.end()) {
                fnAsType = it->second;
            } else {
                auto fnType = std::make_shared<CajetaFunctionType>(
                    module, std::move(paramTypes), std::move(ret), returnsOwn);
                canonicalMap[canon] = static_pointer_cast<CajetaType>(fnType);
                fnAsType = static_pointer_cast<CajetaType>(fnType);
            }
            // `((T)->R)[]`: brackets at this level belong to the OUTER grouping, so
            // wrap the function type in a CajetaArray once per pair.
            CajetaTypePtr type = fnAsType;
            int bracketPairs = static_cast<int>(ctx->LBRACK().size());
            for (int i = 0; i < bracketPairs; i++) {
                type = make_shared<CajetaArray>(module, type);
                module->getStructures()[type->toCanonical()] = static_pointer_cast<CajetaClass>(type);
            }
            return type;
        }
        CajetaTypePtr type;
        QualifiedNamePtr qName;
        CajetaParser::PrimitiveTypeContext* ctxPrimitiveType = ctx->primitiveType();
        if (ctxPrimitiveType != nullptr) {
            qName = QualifiedName::getOrInsert(ctxPrimitiveType->getText(), CAJETA_NATIVE_PACKAGE);
            auto pIt = canonicalMap.find(qName->toCanonical());
            type = pIt == canonicalMap.end() ? nullptr : pIt->second;
        } else {
            CajetaParser::ClassOrInterfaceTypeContext* ctxClassOrInterface = ctx->classOrInterfaceType();
            if (ctxClassOrInterface != nullptr) {
                qName = QualifiedName::fromContext(ctxClassOrInterface);
            } else {
                throw "What is this if not a class or interface?";
            }
            type = resolveNamed(qName, module);
            if (auto* targs = ctxClassOrInterface->typeArguments(0)) {
                if (qName->getTypeName() == "Vector"
                        && targs->typeArgument().size() == 2) {
                    auto* elemArg = targs->typeArgument()[0];
                    auto* lenArg = targs->typeArgument()[1];
                    if (!elemArg->typeType()) {
                        throw Exception(
                            "Vector element type must be a non-bool numeric "
                            "primitive type",
                            "CAJETA_ERROR_VECTOR_ELEMENT_TYPE");
                    }
                    CajetaTypePtr elemT = fromContext(elemArg->typeType(), module);
                    int64_t n = vectorLengthArg(lenArg, module,
                                                "CAJETA_ERROR_VECTOR_LENGTH",
                                                "Vector length N");
                    type = CajetaVector::validateAndCreate(module, elemT, n);
                } else if (qName->getTypeName() == "Matrix"
                        && targs->typeArgument().size() == 3) {
                    // cajeta.math is parsed lazily: the flat CajetaMatrix resolves
                    // without it, but Matrix's operator/method surface lives in
                    // cajeta.math.Matrix, so a bare `Matrix<...>` must pull it in.
                    if (CajetaModule::stdlibImportHook) {
                        CajetaModule::stdlibImportHook("cajeta.math");
                    }
                    // A concrete `Matrix<...>` reference resolves to the flat
                    // row-major CajetaMatrix (`<R*C x T>`) that codegen intercepts.
                    auto* elemArg = targs->typeArgument()[0];
                    auto* rowArg = targs->typeArgument()[1];
                    auto* colArg = targs->typeArgument()[2];
                    if (!elemArg->typeType()) {
                        throw Exception(
                            "Matrix element type must be a non-bool numeric "
                            "primitive type",
                            "CAJETA_ERROR_MATRIX_ELEMENT_TYPE");
                    }
                    CajetaTypePtr elemT = fromContext(elemArg->typeType(), module);
                    int64_t r = vectorLengthArg(rowArg, module,
                                                "CAJETA_ERROR_MATRIX_DIMENSIONS",
                                                "Matrix dimension R");
                    int64_t c = vectorLengthArg(colArg, module,
                                                "CAJETA_ERROR_MATRIX_DIMENSIONS",
                                                "Matrix dimension C");
                    type = CajetaMatrix::validateAndCreate(module, elemT, r, c);
                } else if (qName->getTypeName() == "Quaternion"
                        && targs->typeArgument().size() == 1) {
                    auto* elemArg = targs->typeArgument()[0];
                    if (!elemArg->typeType()) {
                        throw Exception(
                            "Quaternion element type must be a floating-point "
                            "primitive type",
                            "CAJETA_ERROR_QUATERNION_ELEMENT_TYPE");
                    }
                    CajetaTypePtr elemT = fromContext(elemArg->typeType(), module);
                    type = CajetaQuaternion::validateAndCreate(module, elemT);
                } else if (qName->getTypeName() == "Task"
                        && targs->typeArgument().size() == 1) {
                    auto* singleArg = targs->typeArgument()[0];
                    if (singleArg->typeType()) {
                        CajetaTypePtr argType =
                            fromContext(singleArg->typeType(), module);
                        if (!argType) {
                            throw "unresolved Task type argument";
                        }
                        type = CajetaTask::getOrCreate(module, argType);
                    }
                } else {
                    auto templateClass = dynamic_pointer_cast<CajetaClass>(type);
                    // A parameterized `Foo<...>` can only denote a generic class, so
                    // when the bare short-name key landed a NON-template (last writer
                    // wins across packages), re-resolve to a same-named template.
                    if (!templateClass || !templateClass->isTemplate()) {
                        if (auto t = findTemplateByShortName(qName->getTypeName())) {
                            templateClass = dynamic_pointer_cast<CajetaClass>(t);
                            type = t;
                        }
                    }
                    if (templateClass && templateClass->isTemplate()) {
                        vector<CajetaTypePtr> args;
                        for (auto* targ : targs->typeArgument()) {
                            // Grammar `'?' ((EXTENDS|SUPER) typeType)?`: in a bounded
                            // form typeType() is the BOUND, not a regular type arg.
                            if (targ->QUESTION() != nullptr) {
                                if (!CajetaType::wildcardsEnabled()) {
                                    throw "wildcard type arguments not supported in v1";
                                }
                                CajetaTypePtr bound = nullptr;
                                if (targ->typeType() != nullptr) {
                                    bound = fromContext(targ->typeType(), module);
                                    if (!bound) {
                                        throw "unresolved wildcard bound type";
                                    }
                                }
                                CajetaTypePtr wild;
                                if (targ->EXTENDS() != nullptr) {
                                    if (!bound) {
                                        throw "'? extends' must name a bound type";
                                    }
                                    wild = CajetaType::wildcardSentinelExtends(bound);
                                } else if (targ->SUPER() != nullptr) {
                                    if (!bound) {
                                        throw "'? super' must name a bound type";
                                    }
                                    wild = CajetaType::wildcardSentinelSuper(bound);
                                } else {
                                    wild = CajetaType::wildcardSentinel();
                                }
                                if (!wild) {
                                    throw "wildcard sentinel construction failed — CajetaType::init not run?";
                                }
                                args.push_back(wild);
                                continue;
                            }
                            if (targ->integerLiteral() != nullptr) {
                                args.push_back(CajetaConstantType::of(
                                    CajetaConstantType::parseLiteral(
                                        targ->integerLiteral())));
                                continue;
                            }
                            if (!targ->typeType()) {
                                throw "unresolved template argument";
                            }
                            CajetaTypePtr argType = fromContext(targ->typeType(), module);
                            if (!argType) {
                                throw "unresolved template argument";
                            }
                            args.push_back(argType);
                            if (targ->REFERENCE() != nullptr) {
                                throw Exception(
                                    "`#` on a type argument is retired: "
                                    "ownership is per-call under "
                                    "title-tracking (specs/title-tracking-"
                                    "spec.md §8.1) — spell it at the store "
                                    "site (`m.put(#k, #v)`, `xs.add(#x)`) and "
                                    "drop the `#` from `"
                                    + argType->toCanonical() + "`",
                                    "CAJETA_ERROR_TYPE_TRANSFER_RETIRED");
                            }
                        }
                        type = templateClass->instantiate(args);
                    }
                }
            } else {
                // No explicit arguments: a template whose parameters are ALL
                // defaulted resolves bare `Foo` to `Foo<float32>`.
                auto tmpl = dynamic_pointer_cast<CajetaClass>(type);
                if (!tmpl || !tmpl->isTemplate()) {
                    if (auto t = findTemplateByShortName(qName->getTypeName())) {
                        tmpl = dynamic_pointer_cast<CajetaClass>(t);
                    }
                }
                if (tmpl && tmpl->isTemplate()) {
                    const auto& tps = tmpl->getTypeParameters();
                    bool allDefaulted = !tps.empty();
                    for (const auto& p : tps) {
                        if (p.defaultType.empty()) { allDefaulted = false; break; }
                    }
                    if (allDefaulted) {
                        type = tmpl->instantiate({});
                    }
                }
            }
        }
        // Each `[]` pair wraps the type in another CajetaArray; a bracket carrying a
        // CONSTANT size is a fixed-size inline array. An unresolved element type must
        // exit null here -- CajetaArray's ctor dereferences it (segfault, no report).
        if (!type) return nullptr;
        int bracketPairs = static_cast<int>(ctx->LBRACK().size());
        for (int i = 0; i < bracketPairs; i++) {
            int32_t fixedLength = -1;
            if (i < static_cast<int>(ctx->expression().size())) {
                if (auto* sizeExpr = ctx->expression(i)) {
                    fixedLength = parseConstantArrayLength(sizeExpr->getText());
                }
            }
            type = make_shared<CajetaArray>(module, type, fixedLength);
            module->getStructures()[type->toCanonical()] = static_pointer_cast<CajetaClass>(type);
        }

        return type;
    }

    // Recording wrapper over fromContextImpl: the one choke point every type name in
    // the language passes through, so the xref index captures every type reference
    // here. A name in synthesized source resolves to no file and is skipped.
    cajeta::CajetaTypePtr cajeta::CajetaType::fromContext(CajetaParser::TypeTypeContext* ctx, CajetaModulePtr module) {
        CajetaTypePtr resolved = fromContextImpl(ctx, module);
        if (!xref::captureEnabled() || !resolved || !ctx) return resolved;

        auto* tok = ctx->getStart();
        if (!tok || !tok->getInputStream()) return resolved;
        const std::string* file =
            xref::internSourceFile(tok->getInputStream()->getSourceName());
        if (!file) return resolved;

        auto klass = std::dynamic_pointer_cast<CajetaClass>(resolved);
        if (!klass) return resolved;

        // An instantiation has no source of its own; the index carries the template.
        std::string target = klass->getQName()->toCanonical();
        auto lt = target.find('<');
        if (lt != std::string::npos) target = target.substr(0, lt);

        xref::noteTypeReference(target, *file, (int) tok->getLine(),
                                (int) tok->getCharPositionInLine());
        return resolved;
    }

    CajetaTypePtr CajetaType::toPointerType() {
        QualifiedNamePtr pointerName = QualifiedName::getOrInsert(qName->getTypeName() + string("*"),
            qName->getPackageName());
        CajetaTypePtr pointerType = CajetaType::of(pointerName);
        if (!pointerType) {
            pointerType = CajetaType::create(pointerName,
                llvm::PointerType::get(rawLlvmType()->getContext(), 0), POINTER_FLAG);
        }
        return pointerType;
    }

    CajetaTypePtr CajetaType::of(llvm::Type* type, CajetaTypePtr parent) {
        CajetaTypePtr result = nullptr;
        try {
            if (type->isStructTy()) {
                llvm::StringRef ref = type->getStructName();
                if (!ref.empty()) {
                    // find, not operator[]: a miss must not insert a null entry.
                    auto it = canonicalMap.find(ref.str());
                    if (it != canonicalMap.end()) result = it->second;
                }
            } else {
                auto it = typeMap.find(TypeKey(type));
                if (it != typeMap.end()) result = it->second;
            }
        } catch (exception) {
            throw "Exception while mapping value to type";
        }
        return result;
    }

    CajetaTypePtr CajetaType::of(llvm::Value* value, CajetaTypePtr parent) {
        return of(value->getType(), parent);
    }

    // The named struct type in `ctx`, created opaque -- and registered in
    // canonicalMap under `name` -- when it does not exist yet.
    llvm::StructType* CajetaType::getOrCreateLlvmType(llvm::LLVMContext* ctx, string name) {
        llvm::StructType* result = llvm::StructType::getTypeByName(*ctx, name);
        if (result == nullptr) {
            result = llvm::StructType::create(*ctx, name);
            CajetaTypePtr type = CajetaType::create(QualifiedName::getOrCreate(name), result, STRUCT_FLAG);
            canonicalMap[name] = type;
        }
        return result;

    }

    llvm::StructType* CajetaType::getOrCreateLlvmStructNoRegister(llvm::LLVMContext* ctx, const string& name) {
        llvm::StructType* result = llvm::StructType::getTypeByName(*ctx, name);
        if (result == nullptr) {
            result = llvm::StructType::create(*ctx, name);
        }
        return result;
    }

    // As above, but creates the struct with `properties` as its body.
    llvm::StructType* CajetaType::getOrCreateLlvmType(llvm::LLVMContext* ctx, string name, vector<llvm::Type*> properties) {
        llvm::StructType* result = llvm::StructType::getTypeByName(*ctx, name);
        if (result == nullptr) {
            result = llvm::StructType::create(*ctx, llvm::ArrayRef<llvm::Type*>(properties), name);
            CajetaTypePtr type = CajetaType::create(QualifiedName::getOrCreate(name), result, STRUCT_FLAG);
            canonicalMap[name] = type;
        }
        return result;
    }

    /** The CajetaTypeFlags describing `op`'s LLVM type. Vectors key on their
     * ELEMENT type; any otherwise-unmapped type falls back to STRUCT_TYPE_ID. */
    CajetaTypeFlags CajetaType::getTypeFlagsOf(llvm::Value* op) {
        llvm::Type* opType = op->getType();
        if (opType->getTypeID() == llvm::Type::StructTyID) {
            return STRUCT_TYPE_ID;
        }
        // Vectors key on the ELEMENT type -- per-lane arithmetic follows the
        // element's signedness/width. getScalarType() is identity for scalars.
        opType = opType->getScalarType();
        auto it = llvmTypeIdMap.find(opType->getTypeID());
        if (it == llvmTypeIdMap.end() || !it->second) {
            return STRUCT_TYPE_ID;
        }
        return it->second->typeFlags;
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
            } else {
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
                    result = module->getBuilder()->CreateIntCast(op, rawLlvmType(), false);
                    break;
                case INT8_TYPE_ID:
                case INT16_TYPE_ID:
                case INT32_TYPE_ID:
                case INT64_TYPE_ID:
                case INT128_TYPE_ID:
                    result = module->getBuilder()->CreateIntCast(op, rawLlvmType(), true);
                    break;
                case FLOAT4E2M1_TYPE_ID:
                case FLOAT6E2M3_TYPE_ID:
                case FLOAT6E3M2_TYPE_ID:
                case FLOAT8E4M3_TYPE_ID:
                case FLOAT8E5M2_TYPE_ID:
                case FLOAT8E4M3FNUZ_TYPE_ID:
                case FLOAT8E5M2FNUZ_TYPE_ID:
                    throw Exception(string("Casts to sub-fp16 float types require runtime conversion helpers (not yet implemented)."), string("101"));
                case FLOAT16_TYPE_ID:
                case BFLOAT16_TYPE_ID:
                case FLOAT32_TYPE_ID:
                case FLOAT64_TYPE_ID:
                case FLOAT128_TYPE_ID:
                    result = module->getBuilder()->CreateFPCast(op, rawLlvmType());
                    break;
                default:
                    throw Exception(string("Illegal execution error, attempting to normalize non-numeric type."), string("100"));
            }
        }
        return result;
    }
}