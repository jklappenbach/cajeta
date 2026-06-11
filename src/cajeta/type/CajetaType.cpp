//
// Created by James Klappenbach on 10/2/22.
//

#include "CajetaType.h"
#include "../field/Field.h"
#include <cstdlib>
#include <optional>
#include <set>
#include "../compile/CajetaModule.h"
#include "CajetaArray.h"
#include "CajetaCapture.h"
#include "CajetaClass.h"
#include "CajetaTask.h"
#include "CajetaConstantType.h"
#include "CajetaVector.h"
#include "CajetaMatrix.h"
#include "CajetaQuaternion.h"
#include "CajetaFunctionType.h"
#include "../error/InvalidOperandException.h"
#include "../error/Exception.h"

namespace cajeta {

    #define CAJETA_NATIVE_PACKAGE ""
    #define NATIVE_TYPE_ENTRY(typeName, llvmType, typeFlags) CajetaType::create(QualifiedName::getOrInsert(typeName, CAJETA_NATIVE_PACKAGE), llvmType, typeFlags);

    map<string, CajetaTypePtr> CajetaType::canonicalMap;
    map<string, map<string, int32_t>> CajetaType::enumConstants;
    map<TypeKey, CajetaTypePtr> CajetaType::typeMap;
    map<llvm::Type::TypeID, CajetaTypePtr> CajetaType::llvmTypeIdMap;
    // Archive — see CajetaType.h. Cleared by resetGlobals so each
    // fresh Compiler starts with an empty set.
    static map<string, string> g_archive;
    // Side set marking which archived canonical names are enum
    // declarations (rather than classes / interfaces / structs /
    // views). Read by fromContext's placeholder-synthesis path so
    // a cross-file `JsonToken current;` field declaration resolves
    // to the proper i32-backed enum CajetaType instead of a
    // class-shaped placeholder. Populated by the prescan visitor's
    // visitEnumDeclaration override.
    static set<string> g_enumArchive;
    // Archive entries known to be @ValueType classes. Read by
    // fromContext's placeholder-synthesis path so a cross-file
    // value-type-typed declaration gets a placeholder born with
    // VALUE_TYPE_FLAG | BY_VALUE_FLAG. Populated by the prescan
    // visitor's visitClassDeclaration when it sees the annotation.
    static set<string> g_valueTypeArchive;
    // Side set marking which archived canonical names are INTERFACE
    // declarations. Read by fromContext's placeholder-synthesis path so
    // a forward-referenced interface type (referenced by a field/param/
    // local before its own declaration is visited) is born as a fat
    // 24-byte interface pointer instead of a thin class pointer.
    // Populated by the prescan visitor's visitInterfaceDeclaration.
    static set<string> g_interfaceArchive;
    // Per-class template metadata captured by the prescan when the
    // class declaration carries `typeParameters`. Lets the placeholder-
    // synthesis path in fromContext below pre-populate enough state on
    // the placeholder that `isTemplate()` returns true AND a use-site
    // `T<args>` reference can immediately call `placeholder->instantiate(args)`
    // — even though the placeholder's REAL visitClassDeclaration hasn't
    // fired yet. The instantiation re-parses templateSource with the
    // pinned typeParameter names substituted, producing a fully-built
    // `T<args>` class up-front. Without this, the typeArguments at the
    // use site are silently dropped (Box<int32>::get() multi-module bug)
    // because the placeholder's `isTemplate()` returns false.
    //
    // Keyed by canonical name; entries set only for templated classes.
    struct ArchiveTemplateMeta {
        vector<TypeParameter> typeParameters;
        string templateSource;
    };
    static map<string, ArchiveTemplateMeta> g_archiveTemplateMeta;
    // Wildcard feature-flag override (Step 1). Set by tests via
    // CajetaType::setWildcardsEnabledForTest. Null means "fall back
    // to the CAJETA_WILDCARDS env var" (the production path).
    static std::optional<bool> g_wildcardsTestOverride;
    // Step 6 — bounded wildcards. Side table keyed by wildcard
    // canonical name (`?`, `? extends X`, `? super X`). The unbounded
    // entry is registered by init(ctx); bounded entries are created
    // lazily on first parse-site hit. Cleared by resetGlobals.
    struct WildcardInfoEntry {
        CajetaType::WildcardKind kind;
        CajetaTypePtr bound;
    };
    static map<string, WildcardInfoEntry> g_wildcardInfo;


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

    void CajetaType::resetGlobals() {
        canonicalMap.clear();
        typeMap.clear();
        llvmTypeIdMap.clear();
        enumConstants.clear();
        g_archive.clear();
        g_enumArchive.clear();
        g_valueTypeArchive.clear();
        g_interfaceArchive.clear();
        g_archiveTemplateMeta.clear();
        g_wildcardInfo.clear();
        // Test override survives resetGlobals on purpose — a test
        // turning the feature on expects the next Compiler instance
        // to honor that.
    }

    namespace {
        // Snapshot of every global type container, taken once after the
        // pristine stdlib is built (StdlibCache::prime) and reassigned before
        // each reusing test (StdlibCache::restoreBaseline). Holds copies of
        // both the CajetaType member-statics and the file-statics above, so a
        // single restore reverts every user-visible mutation. `valid` guards
        // the production case where a baseline was never captured.
        struct TypeGlobalsBaseline {
            bool valid = false;
            map<string, CajetaTypePtr> canonicalMap;
            map<string, map<string, int32_t>> enumConstants;
            map<TypeKey, CajetaTypePtr> typeMap;
            map<llvm::Type::TypeID, CajetaTypePtr> llvmTypeIdMap;
            map<string, string> g_archive;
            set<string> g_enumArchive;
            set<string> g_valueTypeArchive;
            set<string> g_interfaceArchive;
            map<string, ArchiveTemplateMeta> g_archiveTemplateMeta;
            map<string, WildcardInfoEntry> g_wildcardInfo;
        };
        TypeGlobalsBaseline g_typeBaseline;
    }

    void CajetaType::captureBaseline() {
        g_typeBaseline.canonicalMap = canonicalMap;
        g_typeBaseline.enumConstants = enumConstants;
        g_typeBaseline.typeMap = typeMap;
        g_typeBaseline.llvmTypeIdMap = llvmTypeIdMap;
        g_typeBaseline.g_archive = g_archive;
        g_typeBaseline.g_enumArchive = g_enumArchive;
        g_typeBaseline.g_valueTypeArchive = g_valueTypeArchive;
        g_typeBaseline.g_interfaceArchive = g_interfaceArchive;
        g_typeBaseline.g_archiveTemplateMeta = g_archiveTemplateMeta;
        g_typeBaseline.g_wildcardInfo = g_wildcardInfo;
        g_typeBaseline.valid = true;
    }

    void CajetaType::releaseThrownTransientStructNames() {
        if (!g_typeBaseline.valid) return;
        // A reusing test whose compile THREW (typically an expected-error test)
        // never ran its normal end-of-compile struct-name release, so any USER
        // struct it created keeps its name in the shared LLVMContext — and LLVM
        // struct types are context-owned, outliving the per-test module teardown.
        // Even a fully BODIED user struct is a hazard: TemplateBasicTests
        // .diamondWithoutInferableConstructorThrows builds `test.Holder<int32>`
        // as a 1-field `class Holder<T>` then throws on an un-inferable ctor; a
        // later test re-declaring `interface Holder<T>` (TemplatedInterfaceV2Tests)
        // gets that stale 1-field struct from getOrCreateLlvmType and GEPs into
        // the absent interface vtable/kind slots → "Invalid indices for GEP
        // pointer type". So we release by NAME (not by opaque-ness), walking
        // canonicalMap — the authoritative creation record that also catches
        // structs floating free of any module. We PRESERVE stdlib-resident
        // structs: a stdlib template instantiation accumulated for cross-test
        // reuse lives in the persistent stdlib module and is reused by name, so
        // freeing it would diverge from its accumulated bodies. (The success path
        // keeps its own clearTransientStructNames; this is the throw counterpart.)
        // Production-inert: only the test harness ever captures a baseline.
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
        canonicalMap = g_typeBaseline.canonicalMap;
        enumConstants = g_typeBaseline.enumConstants;
        typeMap = g_typeBaseline.typeMap;
        llvmTypeIdMap = g_typeBaseline.llvmTypeIdMap;
        g_archive = g_typeBaseline.g_archive;
        g_enumArchive = g_typeBaseline.g_enumArchive;
        g_valueTypeArchive = g_typeBaseline.g_valueTypeArchive;
        g_interfaceArchive = g_typeBaseline.g_interfaceArchive;
        g_archiveTemplateMeta = g_typeBaseline.g_archiveTemplateMeta;
        g_wildcardInfo = g_typeBaseline.g_wildcardInfo;
    }

    map<string, string>& CajetaType::getArchive() { return g_archive; }

    void CajetaType::markArchiveEnum(const string& canonical) {
        g_enumArchive.insert(canonical);
    }

    bool CajetaType::isArchiveEnum(const string& canonical) {
        return g_enumArchive.count(canonical) > 0;
    }

    void CajetaType::markArchiveValueType(const string& canonical) {
        g_valueTypeArchive.insert(canonical);
    }

    bool CajetaType::isArchiveValueType(const string& canonical) {
        return g_valueTypeArchive.count(canonical) > 0;
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
        // Default ON as of Step 5b — stdlib stream wrappers use
        // `Stream<?>` overrides for chain-walking and the stdlib parses
        // on every compile, so wildcards must be accepted by default
        // for the runtime to load. CAJETA_WILDCARDS=0 is the backout
        // opt-out for emergencies.
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

    // Bounded-wildcard sentinel factory. Lazily creates a per-(kind, bound)
    // CajetaType keyed by canonical "? <kind-word> <bound-canonical>".
    // Shares the unbounded sentinel's llvm type (opaque pointer); bound
    // info goes into g_wildcardInfo for query by wildcardBound().
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
        // Phase 1.2 — extends-bounded capture projects to its upper
        // bound at read positions. Mirrors the bounded-wildcard case
        // below; future code paths that build captures at binding
        // sites (Phase 2+) feed this same projection to chained
        // member access.
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
        // Both keys point at the canonical so the miss-path in
        // fromContext can promote a bare short name into the right
        // qualified placeholder. First write wins; a second
        // registration of the same canonical is fine (idempotent
        // from re-running the pre-scan), and a second registration
        // of the same SHORT name with a different canonical means
        // two classes share the short name across packages — keep
        // the first to remain deterministic, and let the visitor
        // catch the duplicate-canonical case at parse time if any.
        if (!canonical.empty() && g_archive.count(canonical) == 0) {
            g_archive[canonical] = canonical;
        }
        if (!shortName.empty() && g_archive.count(shortName) == 0) {
            g_archive[shortName] = canonical;
        }
    }

    void CajetaType::init(llvm::LLVMContext& ctx) {
        NATIVE_TYPE_ENTRY("void", llvm::Type::getVoidTy(ctx), VOID_TYPE_ID);
        NATIVE_TYPE_ENTRY("boolean", llvm::Type::getInt1Ty(ctx), BOOLEAN_TYPE_ID);
        // `char` is the **Unicode codepoint** type (32-bit signed),
        // not the C-style 8-bit byte. Cajeta's `char` matches Go's
        // `rune` semantically — a single Unicode code point — and the
        // character literal `'c'` evaluates to int32 99, `'é'` to 233,
        // `'😀'` to 0x1F600. The 8-bit byte type has perfectly good
        // names (`int8` / `uint8`); `char` doesn't need to alias them.
        // See docs/stdlib/lang/String.md § `char` is a 32-bit
        // Unicode codepoint. Redefined 2026-05-18 (was i8).
        //
        // `uchar` is kept as a deprecated alias for `uint8` so legacy
        // code referencing the 8-bit name keeps compiling; new code
        // should use `uint8` directly. shareLlvmType=false keeps the
        // reverse i8 -> name lookup landing on `uint8`.
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
        // Sub-byte and 8-bit floats from the OCP Microscaling spec. LLVM has no IR-level
        // Type* for these formats (only APFloat semantics), so we represent them as iN
        // storage and rely on runtime helpers for conversions/arithmetic (future work).
        // shareLlvmType=false so the iN registration doesn't overwrite the int{4,6,8} entries.
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
        // `float16` is IEEE-754 binary16 (LLVM `half`) — the standard meaning of
        // "float16", and the element format the hardware cooperative-matrix path
        // (SPV_KHR_cooperative_matrix → OpTypeFloat 16) and the RADV WMMA f16→f32
        // device config require. (bfloat16 is a *distinct* format with the same
        // bit-width but a wider exponent; it gets its own future `bfloat16` keyword
        // and maps to LLVM `bfloat`. Don't conflate the two.)
        NATIVE_TYPE_ENTRY("float16", llvm::Type::getHalfTy(ctx), FLOAT16_TYPE_ID);
        // bfloat16 — the brain-float ML dtype (LLVM `bfloat`): 16-bit, float32's
        // 8-bit exponent, fewer mantissa bits. Distinct from float16 (binary16).
        NATIVE_TYPE_ENTRY("bfloat16", llvm::Type::getBFloatTy(ctx), BFLOAT16_TYPE_ID);
        NATIVE_TYPE_ENTRY("float32", llvm::Type::getFloatTy(ctx), FLOAT32_TYPE_ID);
        NATIVE_TYPE_ENTRY("float64", llvm::Type::getDoubleTy(ctx), FLOAT64_TYPE_ID);
        NATIVE_TYPE_ENTRY("float128", llvm::Type::getFP128Ty(ctx), FLOAT128_TYPE_ID);
        NATIVE_TYPE_ENTRY("pointer", llvm::PointerType::get(ctx, 0), POINTER_TYPE_ID);
        // Wildcard sentinel (`?`) — Step 1 of template wildcards. Always
        // registered regardless of feature-flag state so other passes
        // can rely on `wildcardSentinel()` being non-null; the flag
        // gates only the parser path that *produces* it as an arg.
        // STRUCT_FLAG (no PRIMITIVE_FLAG) so toGeneric() returns the
        // canonical "?" rather than misclassifying it as a pointer
        // primitive. shareLlvmType=false so its opaque-pointer backing
        // doesn't overwrite the canonical `pointer` entry in typeMap /
        // llvmTypeIdMap.
        CajetaType::create(
            QualifiedName::getOrInsert("?", CAJETA_NATIVE_PACKAGE),
            llvm::PointerType::get(ctx, 0),
            STRUCT_FLAG,
            /*shareLlvmType=*/false);
        g_wildcardInfo["?"] = {CajetaType::WildcardKind::Unbounded, nullptr};
        // Phase 2b-β: the legacy primitive-alias `String` (an i8*
        // C-string) is RETIRED. The `cajeta.lang.String` class registers
        // itself in canonicalMap under both the canonical and short name
        // when the runtime parses — every reference to `String` now
        // resolves to the class. Any compiler code that needs a raw
        // C-string spell `pointer` (or, more rigorously, the new
        // encoding-prefixed byte-array literal — task #164, L-29 in
        // Features.md — once shipped). See
        // docs/stdlib/lang/String.md § Memory model.
    }

    llvm::ConstantInt* CajetaType::getTypeAllocSize(CajetaModulePtr module) {
        const llvm::DataLayout& dataLayout = module->getLlvmModule()->getDataLayout();
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*module->getLlvmContext()),
            dataLayout.getTypeAllocSize(llvmType));
    }

    string CajetaType::toGeneric() {
        if (typeFlags & PRIMITIVE_FLAG) {
            // getLlvmType() (not the raw llvmType field) so a lazily-built type
            // is resolved rather than dereferenced null — the intrinsic
            // aggregate value types CajetaVector / CajetaMatrix start with a
            // null llvmType and only build it on demand.
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
                    // Vector<T,N> / Matrix<T,R,C>: distinct shapes are distinct
                    // generic tokens (their canonical name), not all "number".
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
        // Stale instance: resolve to the canonical class object, which carries
        // the bit once generatePrototype has run. See the header doc-comment.
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
        return CajetaType::canonicalMap[qName->toCanonical()];
    }

    CajetaTypePtr CajetaType::of(string typeName, string package) {
        QualifiedNamePtr qName = QualifiedName::getOrInsert(typeName, package);
        return CajetaType::canonicalMap[qName->toCanonical()];
    }

    CajetaTypePtr CajetaType::of(QualifiedNamePtr qName) {
        return CajetaType::canonicalMap[qName->toCanonical()];
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
        // Fall back to the active module set during the walk — many parse-time
        // call sites don't have a `module` to pass. See CajetaModule::activeModule.
        if (!module) {
            module = CajetaModule::getActiveModule();
        }
        // Function type: `(T1, T2) -> R`. Resolve each component and build
        // (or look up by canonical) a CajetaFunctionType. See
        // docs/stdlib/Lambdas.md. The return slot is typeTypeOrVoid so
        // `(T) -> void` is a legal function-type shape (P6.5).
        if (auto* fnt = ctx->functionType()) {
            std::vector<CajetaTypePtr> paramTypes;
            for (auto* p : fnt->typeType()) {
                paramTypes.push_back(fromContext(p, module));
            }
            CajetaTypePtr ret;
            // M5(b) — function-type return ABI discriminator. Source-level
            // `(P) -> #R` (REFERENCE present) means the ownership/heap
            // pointer-return form `R* (params)`; `(P) -> R` (no `#`) means
            // the sret value-return form `void (ptr sret(R), params)`. The
            // distinction is only meaningful when the return type can be
            // sret-shaped (class, non-interface, non-array, non-view) —
            // CajetaFunctionType's canonical-build normalizes non-eligible
            // returns to ownership so primitive/void/interface fn-types
            // don't get spurious canonical splits.
            bool returnsOwn = true;
            if (auto* rt = fnt->typeTypeOrVoid()) {
                if (rt->VOID()) {
                    // Resolve void via canonicalMap (registered by the
                    // primitive-bootstrap path).
                    QualifiedNamePtr voidQ = QualifiedName::getOrInsert(
                        "void", CAJETA_NATIVE_PACKAGE);
                    ret = canonicalMap[voidQ->toCanonical()];
                } else if (rt->typeType()) {
                    ret = fromContext(rt->typeType(), module);
                    returnsOwn = (rt->REFERENCE() != nullptr);
                }
            }
            // ret may be null when the return slot names an unknown
            // identifier — pre-P6.5 fromContext returned null for an
            // unresolved name without throwing, and downstream codegen
            // (e.g. method-reference NOT_IMPLEMENTED) surfaced the
            // original problem. Preserve that shape: build the
            // CajetaFunctionType with a null return; buildCanonical
            // already handles it ("?" slot).
            std::string canon = CajetaFunctionType::buildCanonical(paramTypes, ret, returnsOwn);
            auto it = canonicalMap.find(canon);
            if (it != canonicalMap.end()) return it->second;
            auto fnType = std::make_shared<CajetaFunctionType>(
                module, std::move(paramTypes), std::move(ret), returnsOwn);
            canonicalMap[canon] = static_pointer_cast<CajetaType>(fnType);
            return static_pointer_cast<CajetaType>(fnType);
        }
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
            // Template type-parameter substitution: when we're inside a template
            // instantiation walk, `T` should resolve to whatever concrete type
            // was bound for this instantiation (consulted via the module's
            // substitution stack). Only matched on the simple type name —
            // template parameters are unqualified by definition.
            if (module) {
                CajetaTypePtr substituted = module->lookupTypeParameter(qName->getTypeName());
                if (substituted) {
                    type = substituted;
                }
            }
            if (!type) {
                auto it = canonicalMap.find(qName->toCanonical());
                if (it != canonicalMap.end()) {
                    type = it->second;
                } else if (module && qName->getPackageName().empty()) {
                    // Import-aware short-name resolution. The user
                    // wrote a bare type name (no package qualifier);
                    // check the active module's imports map. The map
                    // shape is imports[shortName][packageName] = qn,
                    // populated by onImportDeclaration. A hit gives
                    // us the fully-qualified canonical to look up,
                    // disambiguating between same-short-name classes
                    // in different packages. Doing this BEFORE the
                    // short-name fallback below is what makes
                    // `import a.b.Foo;` actually steer resolution.
                    auto& imports = module->getImports();
                    auto importIt = imports.find(qName->getTypeName());
                    if (importIt != imports.end() && !importIt->second.empty()) {
                        // First entry wins — multiple imports of
                        // the same short name from different packages
                        // is a future ambiguity-error condition, not
                        // a quiet pick. v1 just takes one
                        // deterministically.
                        auto& imported = importIt->second.begin()->second;
                        auto canonIt = canonicalMap.find(imported->toCanonical());
                        if (canonIt != canonicalMap.end()) {
                            type = canonIt->second;
                        }
                    }
                    if (!type) {
                        // Fall back to the native ("") package —
                        // covers built-in aliases like
                        // String/Exception that fromContext defaults
                        // to package "code".
                        auto nativeIt = canonicalMap.find(qName->getTypeName());
                        if (nativeIt != canonicalMap.end()) {
                            type = nativeIt->second;
                        }
                    }
                } else {
                    auto nativeIt = canonicalMap.find(qName->getTypeName());
                    if (nativeIt != canonicalMap.end()) {
                        type = nativeIt->second;
                    }
                }
                // Placeholder synthesis. The four lookup tiers
                // above couldn't find a defined type, but the
                // archive pre-scan may have noted that the name
                // IS declared somewhere in the compilation unit
                // — just not yet visited by the body walk that
                // would have populated canonicalMap. Create a
                // forward-reference placeholder CajetaClass under
                // the archive's canonical so the caller carries
                // a real CajetaTypePtr forward. visitClassDeclaration
                // later detects the existing placeholder and fills
                // it in on the same shared_ptr.
                if (!type) {
                    auto& archive = getArchive();
                    std::string lookup = qName->toCanonical();
                    auto archIt = archive.find(lookup);
                    if (archIt == archive.end()) {
                        // Try short name — archive carries both keys
                        // so bare-name references can still vouch.
                        archIt = archive.find(qName->getTypeName());
                    }
                    if (archIt != archive.end()) {
                        const std::string& canonical = archIt->second;
                        // Parse canonical into package + short.
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
                            // Enum: synthesize the i32-backed
                            // primitive type directly. Mirrors the
                            // visitEnumDeclaration registration shape
                            // so a cross-file `JsonToken current;`
                            // field declaration gets the correct
                            // layout (i32 slot) instead of a class-
                            // shaped placeholder. Enum constants
                            // (JsonToken.END, etc.) are populated
                            // when the enum's own visitEnumDeclaration
                            // body walk runs; this just makes the
                            // TYPE available before that point.
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
                            // Forward-referenced NON-generic interface: born
                            // FAT. A field/param/local declared at this
                            // interface type before the interface's own
                            // declaration is visited must lay out as a 24-byte
                            // fat pointer `{ ptr data, ptr vtable, i64 kind }`,
                            // not a thin 8-byte class pointer — otherwise the
                            // owning class's layout reserves the wrong width and
                            // interface dispatch through the member is silently
                            // dropped at codegen (no `call` emitted → garbage
                            // result). Build the named struct body eagerly here
                            // (keyed by canonical, so the interface's real
                            // generatePrototype reuses the SAME StructType) and
                            // tag the placeholder isInterface=true so
                            // fieldLayoutType + the interface-dispatch path take
                            // the fat branch. The real visitInterfaceDeclaration
                            // later fills this SAME shared_ptr with the method
                            // set (placeholder reuse), so every earlier
                            // reference picks up the dispatch slots.
                            // (Generic interfaces fall through to the class
                            // placeholder path below, which handles
                            // typeParameters / instantiation.)
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
                            // getOrCreateLlvmType inserted a plain CajetaType
                            // for `canonical`; overwrite both keys so name
                            // lookups land the interface CajetaClass (a
                            // class→interface upcast at a call site needs the
                            // formal's type to dynamic_cast to CajetaClass).
                            canonicalMap[canonical] = placeholder;
                            canonicalMap[shortName] = placeholder;
                            type = placeholder;
                        } else {
                            auto placeholder = std::make_shared<CajetaClass>(
                                module, phName,
                                std::list<QualifiedNamePtr>{},
                                std::list<QualifiedNamePtr>{});
                            placeholder->setPlaceholder(true);
                            // Born-correct @ValueType: if the prescan saw
                            // this class annotated @ValueType, the placeholder
                            // gets VALUE_TYPE_FLAG | BY_VALUE_FLAG NOW, before
                            // generatePrototype runs on the canonical. This is
                            // the stale-instance fix: any AST node that captures
                            // this placeholder (a `Vec2 a;` local-var type) sees
                            // the by-value storage axis directly, so StackField /
                            // LocalVariableDeclaration allocate an inline slot
                            // without a canonical-map backstop. Mirrors the
                            // isArchiveEnum i32 path above.
                            if (isArchiveValueType(canonical)) {
                                placeholder->addTypeFlags(
                                    VALUE_TYPE_FLAG | BY_VALUE_FLAG);
                            }
                            // Template-metadata seeding. If the prescan
                            // captured typeParameters + source for this
                            // class, install them on the placeholder now —
                            // then `placeholder->isTemplate()` returns
                            // true and a use-site `T<args>` reference can
                            // immediately call instantiate(args), instead
                            // of silently dropping the type-args. The
                            // real visitClassDeclaration later overwrites
                            // both fields with identical values from the
                            // real parse (harmless).
                            if (const auto* archParams =
                                    lookupArchiveTemplateParameters(canonical)) {
                                placeholder->setTypeParameters(*archParams);
                                if (const auto* archSrc =
                                        lookupArchiveTemplateSource(canonical)) {
                                    placeholder->setTemplateSource(*archSrc);
                                }
                            }
                            // Don't pre-set llvmType — leave it null so
                            // the real generatePrototype's
                            // getOrCreateLlvmType call creates a named
                            // StructType under the canonical (rather
                            // than seeing a pre-set non-struct type and
                            // discarding the name). CajetaClass::getLlvmType
                            // overrides to return `ptr` while the
                            // placeholder is unfilled, so earlier-parsed
                            // classes composing layouts against the
                            // placeholder still get a sized type back.
                            canonicalMap[canonical] = placeholder;
                            canonicalMap[shortName] = placeholder;
                            type = placeholder;
                        }
                    }
                }
            }
            // Template instantiation: if the type-use site carries
            // typeArguments (e.g. `Box<int32>`), resolve them and route
            // through the template's instantiate(...) cache. Each argument
            // is itself a typeType and goes through fromContext recursively
            // — substitutions cascade naturally, so `Pair<int32, T>` inside
            // an outer template walk lands `T` to its current substitution
            // before instantiating Pair.
            //
            // Diamond operator (`Box<>`) is parsed as typeArgumentsOrDiamond
            // but doesn't appear in the typeType production — it only shows
            // up under classCreatorRest's createdName. So here we only see
            // explicit-args typeArguments; diamond inference is TPL-7's job
            // and lives in NewExpression / ClassCreatorRest.
            if (auto* targs = ctxClassOrInterface->typeArguments(0)) {
                // Built-in Task<T>: synthesized type, not a user template.
                // CajetaTask::getOrCreate(module, T) materializes a fresh
                // CajetaTask per (module, T) and caches it on the module
                // structure map. Handled here — before the generic
                // template path — so we don't need to register a fake
                // "Task" template class just to satisfy the
                // isTemplate() check. See docs/AsyncStatus.md §
                // Plan: Task<T> as user-typeable template.
                if (qName->getTypeName() == "Vector"
                        && targs->typeArgument().size() == 2) {
                    // Built-in Vector<T, N> — a value vector lowering to
                    // <N x T>. arg0: element type (a non-bool numeric
                    // primitive). arg1: a positive integer-constant lane
                    // count. Synthesized here (like Task) before the generic
                    // template path; the element-numeric / N-positive
                    // constraints are checked directly since Vector is not a
                    // user template that would run TemplateInstantiator's bound
                    // check. See CajetaVector.
                    auto* elemArg = targs->typeArgument()[0];
                    auto* lenArg = targs->typeArgument()[1];
                    if (!elemArg->typeType()) {
                        throw Exception(
                            "Vector element type must be a non-bool numeric "
                            "primitive type",
                            "CAJETA_ERROR_VECTOR_ELEMENT_TYPE");
                    }
                    CajetaTypePtr elemT = fromContext(elemArg->typeType(), module);
                    if (lenArg->integerLiteral() == nullptr) {
                        throw Exception(
                            "Vector length N must be a positive integer "
                            "literal constant",
                            "CAJETA_ERROR_VECTOR_LENGTH");
                    }
                    int64_t n = CajetaConstantType::parseLiteral(
                        lenArg->integerLiteral());
                    // Semantic checks (numeric/non-bool element, positive N)
                    // are shared with the construction path. See CajetaVector.
                    type = CajetaVector::validateAndCreate(module, elemT, n);
                } else if (qName->getTypeName() == "Matrix"
                        && targs->typeArgument().size() == 3) {
                    // Built-in Matrix<T, R, C> (B1) — the HYBRID value type. The
                    // declared cajeta.math.Matrix class supplies the operator/
                    // method surface (and was already resolved into `type` by the
                    // short-name fallback above), but a concrete `Matrix<...>`
                    // REFERENCE resolves to the flat row-major CajetaMatrix
                    // representation (`<R*C x T>`), which codegen intercepts —
                    // identical generated code to a pure-intrinsic vector. arg0:
                    // element type (a non-bool numeric primitive); arg1/arg2:
                    // positive integer-constant row/col counts. See CajetaMatrix.
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
                    if (rowArg->integerLiteral() == nullptr ||
                            colArg->integerLiteral() == nullptr) {
                        throw Exception(
                            "Matrix dimensions R and C must be positive integer "
                            "literal constants",
                            "CAJETA_ERROR_MATRIX_DIMENSIONS");
                    }
                    int64_t r = CajetaConstantType::parseLiteral(
                        rowArg->integerLiteral());
                    int64_t c = CajetaConstantType::parseLiteral(
                        colArg->integerLiteral());
                    type = CajetaMatrix::validateAndCreate(module, elemT, r, c);
                } else if (qName->getTypeName() == "Quaternion"
                        && targs->typeArgument().size() == 1) {
                    // Built-in Quaternion<T> — a value quaternion lowering to
                    // `<4 x T>` (w, x, y, z). arg0: a floating-point element
                    // type. Synthesized here (like Vector/Matrix); the
                    // float-element constraint is checked in validateAndCreate.
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
                    // Same-short-name collision guard. A parameterized
                    // reference `Foo<...>` can ONLY denote a generic class —
                    // you cannot parameterize a non-generic one. If the bare
                    // short-name fallback above landed a NON-template (e.g.
                    // unqualified `Stream<T>` resolved to the final,
                    // non-generic `cajeta.xpu.core.Stream` instead of the
                    // generic `cajeta.lang.stream.Stream`, because both
                    // register the bare key "Stream" in the process-global
                    // canonicalMap and the last writer wins), re-resolve to a
                    // same-short-name TEMPLATE. Without this the type
                    // arguments are silently dropped and the intended generic
                    // type/parent is lost — which broke every
                    // `ArrayStream<T>`-derived stream (fold/map/reduce/…) once
                    // `cajeta.xpu.core.Stream` was added to the build.
                    if (!templateClass || !templateClass->isTemplate()) {
                        if (auto t = findTemplateByShortName(qName->getTypeName())) {
                            templateClass = dynamic_pointer_cast<CajetaClass>(t);
                            type = t;
                        }
                    }
                    if (templateClass && templateClass->isTemplate()) {
                        vector<CajetaTypePtr> args;
                        for (auto* targ : targs->typeArgument()) {
                            // Wildcard branch — `?`, `? extends T`, or
                            // `? super T`. Grammar
                            // `'?' ((EXTENDS|SUPER) typeType)?` means
                            // typeType() carries the BOUND for bounded
                            // forms (not a regular type arg). Step 6
                            // — see docs/TemplateWildcard.md.
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
                                // Non-type (integer constant) template
                                // argument — the `N` in `Vector<T, N>` and,
                                // latently, any future user template that
                                // takes a `uint32 N` parameter. Carried as a
                                // CajetaConstantType so it flows through the
                                // existing vector<CajetaTypePtr> + cache-key
                                // machinery unchanged.
                                args.push_back(CajetaConstantType::of(
                                    CajetaConstantType::parseLiteral(
                                        targ->integerLiteral())));
                                continue;
                            }
                            if (!targ->typeType()) {
                                // Bare `primitiveType` alternative.
                                // typeType already subsumes primitives,
                                // so this branch is effectively dead.
                                throw "unresolved template argument";
                            }
                            CajetaTypePtr argType = fromContext(targ->typeType(), module);
                            if (!argType) {
                                throw "unresolved template argument";
                            }
                            args.push_back(argType);
                        }
                        type = templateClass->instantiate(args);
                    }
                }
            }
        }
        // Each `[]` pair wraps the type in another CajetaArray. `int[]` -> CajetaArray<int>;
        // `int[][]` -> CajetaArray<CajetaArray<int>>. The size expressions (when present)
        // are allocation-time concerns; they don't change the type.
        int bracketPairs = static_cast<int>(ctx->LBRACK().size());
        for (int i = 0; i < bracketPairs; i++) {
            type = make_shared<CajetaArray>(module, type);
            module->getStructures()[type->toCanonical()] = static_pointer_cast<CajetaClass>(type);
        }

        return type;
    }

    CajetaTypePtr CajetaType::toPointerType() {
        QualifiedNamePtr pointerName = QualifiedName::getOrInsert(qName->getTypeName() + string("*"),
            qName->getPackageName());
        CajetaTypePtr pointerType = CajetaType::of(pointerName);
        if (!pointerType) {
            pointerType = CajetaType::create(pointerName,
                llvm::PointerType::get(llvmType->getContext(), 0), POINTER_FLAG);
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
                case FLOAT4E2M1_TYPE_ID:
                case FLOAT6E2M3_TYPE_ID:
                case FLOAT6E3M2_TYPE_ID:
                case FLOAT8E4M3_TYPE_ID:
                case FLOAT8E5M2_TYPE_ID:
                case FLOAT8E4M3FNUZ_TYPE_ID:
                case FLOAT8E5M2FNUZ_TYPE_ID:
                    // LLVM has no IR-level Type* for these formats. Storage is iN; casts to/from
                    // standard FP types need runtime conversion helpers (not yet implemented).
                    throw Exception(string("Casts to sub-fp16 float types require runtime conversion helpers (not yet implemented)."), string("101"));
                case FLOAT16_TYPE_ID:
                case BFLOAT16_TYPE_ID:
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