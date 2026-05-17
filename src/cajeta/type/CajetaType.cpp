//
// Created by James Klappenbach on 10/2/22.
//

#include "CajetaType.h"
#include "../field/Field.h"
#include "../compile/CajetaModule.h"
#include "CajetaArray.h"
#include "CajetaClass.h"
#include "CajetaTask.h"
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
    }

    map<string, string>& CajetaType::getArchive() { return g_archive; }

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
        // `char` and `uchar` are the historical 8-bit names; `int8` / `uint8`
        // are the width-uniform names matching the rest of the int* family
        // (and what the README documents). All four share the same LLVM
        // type — pass shareLlvmType=false on the aliases so the reverse
        // i8 -> name lookup keeps `char`/`uchar` canonical (avoids
        // gratuitous rename in error messages).
        NATIVE_TYPE_ENTRY("uchar", llvm::Type::getInt8Ty(ctx), UINT8_TYPE_ID);
        NATIVE_TYPE_ENTRY("char", llvm::Type::getInt8Ty(ctx), INT8_TYPE_ID);
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
        NATIVE_TYPE_ENTRY("float16", llvm::Type::getBFloatTy(ctx), FLOAT16_TYPE_ID);
        NATIVE_TYPE_ENTRY("float32", llvm::Type::getFloatTy(ctx), FLOAT32_TYPE_ID);
        NATIVE_TYPE_ENTRY("float64", llvm::Type::getDoubleTy(ctx), FLOAT64_TYPE_ID);
        NATIVE_TYPE_ENTRY("float128", llvm::Type::getFP128Ty(ctx), FLOAT128_TYPE_ID);
        NATIVE_TYPE_ENTRY("pointer", llvm::PointerType::get(ctx, 0), POINTER_TYPE_ID);
        // `String` is an alias for the opaque-pointer type today — string literals
        // are global-string-ptr (i8*) and there's no String class yet. Registered
        // with shareLlvmType=false so the reverse lookup keeps "pointer" canonical.
        CajetaType::create(
            QualifiedName::getOrInsert("String", CAJETA_NATIVE_PACKAGE),
            llvm::PointerType::get(ctx, 0), POINTER_TYPE_ID, /*shareLlvmType=*/false);
    }

    llvm::ConstantInt* CajetaType::getTypeAllocSize(CajetaModulePtr module) {
        const llvm::DataLayout& dataLayout = module->getLlvmModule()->getDataLayout();
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*module->getLlvmContext()),
            dataLayout.getTypeAllocSize(llvmType));
    }

    string CajetaType::toGeneric() {
        if (typeFlags & PRIMITIVE_FLAG) {
            switch (llvmType->getTypeID()) {
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
        // Fall back to the active module set during the walk — many parse-time
        // call sites don't have a `module` to pass. See CajetaModule::activeModule.
        if (!module) {
            module = CajetaModule::getActiveModule();
        }
        // Function type: `(T1, T2) -> R`. Resolve each component and build
        // (or look up by canonical) a CajetaFunctionType. See
        // cajeta-docs/Lambdas.md. The return slot is typeTypeOrVoid so
        // `(T) -> void` is a legal function-type shape (P6.5).
        if (auto* fnt = ctx->functionType()) {
            std::vector<CajetaTypePtr> paramTypes;
            for (auto* p : fnt->typeType()) {
                paramTypes.push_back(fromContext(p, module));
            }
            CajetaTypePtr ret;
            if (auto* rt = fnt->typeTypeOrVoid()) {
                if (rt->VOID()) {
                    // Resolve void via canonicalMap (registered by the
                    // primitive-bootstrap path).
                    QualifiedNamePtr voidQ = QualifiedName::getOrInsert(
                        "void", CAJETA_NATIVE_PACKAGE);
                    ret = canonicalMap[voidQ->toCanonical()];
                } else if (rt->typeType()) {
                    ret = fromContext(rt->typeType(), module);
                }
            }
            // ret may be null when the return slot names an unknown
            // identifier — pre-P6.5 fromContext returned null for an
            // unresolved name without throwing, and downstream codegen
            // (e.g. method-reference NOT_IMPLEMENTED) surfaced the
            // original problem. Preserve that shape: build the
            // CajetaFunctionType with a null return; buildCanonical
            // already handles it ("?" slot).
            std::string canon = CajetaFunctionType::buildCanonical(paramTypes, ret);
            auto it = canonicalMap.find(canon);
            if (it != canonicalMap.end()) return it->second;
            auto fnType = std::make_shared<CajetaFunctionType>(
                module, std::move(paramTypes), std::move(ret));
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
                        auto placeholder = std::make_shared<CajetaClass>(
                            module, phName,
                            std::list<QualifiedNamePtr>{},
                            std::list<QualifiedNamePtr>{});
                        placeholder->setPlaceholder(true);
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
                // isTemplate() check. See cajeta-docs/AsyncStatus.md §
                // Plan: Task<T> as user-typeable template.
                if (qName->getTypeName() == "Task"
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
                    if (templateClass && templateClass->isTemplate()) {
                        vector<CajetaTypePtr> args;
                        for (auto* targ : targs->typeArgument()) {
                            if (!targ->typeType()) {
                                throw "wildcard type arguments not supported in v1";
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