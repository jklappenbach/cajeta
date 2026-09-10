//
// Created by James Klappenbach on 4/19/23.
//

#include "NewExpression.h"
#include "CreatorRest.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/xref/XrefIndex.h"
#include "cajeta/type/CajetaArray.h"
#include "cajeta/type/CajetaVector.h"
#include "cajeta/type/CajetaMatrix.h"
#include "cajeta/type/CajetaQuaternion.h"
#include "cajeta/type/CajetaConstantType.h"
#include "cajeta/type/VectorOps.h"
#include "cajeta/type/MatrixOps.h"
#include "cajeta/error/Exception.h"

namespace cajeta {
    // Built-in Vector<T,N> -> the CajetaVector value type; construction yields an
    // SSA `<N x T>`, so `new`/`stack` on one is purely syntactic.
    static CajetaVectorPtr resolveVectorNew(
            CajetaModulePtr module, const string& typeName,
            const vector<CajetaTypePtr>& typeArguments) {
        if (typeName != "Vector" || typeArguments.size() != 2) return nullptr;
        auto cv = dynamic_pointer_cast<CajetaConstantType>(typeArguments[1]);
        if (!cv) {
            throw Exception(
                "Vector length N must be a positive integer constant",
                "CAJETA_ERROR_VECTOR_LENGTH");
        }
        return CajetaVector::validateAndCreate(module, typeArguments[0],
                                               cv->getValue());
    }

    // Built-in Matrix<T,R,C> -> the flat CajetaMatrix value type; mirrors
    // resolveVectorNew, and its R*C arguments fill the matrix row by row.
    static CajetaMatrixPtr resolveMatrixNew(
            CajetaModulePtr module, const string& typeName,
            const vector<CajetaTypePtr>& typeArguments) {
        if (typeName != "Matrix" || typeArguments.size() != 3) return nullptr;
        auto rv = dynamic_pointer_cast<CajetaConstantType>(typeArguments[1]);
        auto cv = dynamic_pointer_cast<CajetaConstantType>(typeArguments[2]);
        if (!rv || !cv) {
            throw Exception(
                "Matrix dimensions R and C must be positive integer constants",
                "CAJETA_ERROR_MATRIX_DIMENSIONS");
        }
        return CajetaMatrix::validateAndCreate(module, typeArguments[0],
                                               rv->getValue(), cv->getValue());
    }

    // Built-in Quaternion<T> -> the flat CajetaQuaternion value type, built from
    // four scalar arguments (w, x, y, z).
    static CajetaQuaternionPtr resolveQuaternionNew(
            CajetaModulePtr module, const string& typeName,
            const vector<CajetaTypePtr>& typeArguments) {
        if (typeName != "Quaternion" || typeArguments.size() != 1) return nullptr;
        return CajetaQuaternion::validateAndCreate(module, typeArguments[0]);
    }

    // Bare construction of an all-defaulted template — `heap Box(args)` for
    // `class Box<T = int32>` -> Box<int32>. Any other `type` comes back unchanged.
    static CajetaTypePtr defaultedInstantiation(CajetaTypePtr type,
                                                const string& typeName) {
        auto klass = dynamic_pointer_cast<CajetaClass>(type);
        if (!klass || !klass->isTemplate()) {
            if (auto t = CajetaType::findTemplateByShortName(typeName)) {
                klass = dynamic_pointer_cast<CajetaClass>(t);
            }
        }
        if (!klass || !klass->isTemplate()) return type;
        const auto& tps = klass->getTypeParameters();
        if (tps.empty()) return type;
        for (const auto& p : tps) {
            if (p.defaultType.empty()) return type;
        }
        return klass->instantiate({});
    }

    void NewExpression::recordCreatedTypeXref(antlr4::Token* tok) {
        if (!xref::captureEnabled() || typeName.empty() || !tok) return;
        if (!tok->getInputStream()) return;
        const std::string* file =
            xref::internSourceFile(tok->getInputStream()->getSourceName());
        if (!file) return;
        try {
            // Scoped like resolveTypes and tolerant of a forward reference: the
            // whole-root export reaches `heap Square(...)` before Square.cajeta.
            CajetaModulePtr module = CajetaModule::getActiveModule();
            std::string target;
            if (!package.empty()) {
                std::string qualified = package + "." + typeName;
                if (CajetaType::getArchive().count(qualified)) target = qualified;
            }
            if (target.empty())
                target = CajetaType::canonicalNameScoped(typeName, module);
            if (target.empty()) return;
            // An instantiation navigates to the TEMPLATE's declaration.
            auto lt = target.find('<');
            if (lt != std::string::npos) target = target.substr(0, lt);
            xref::noteTypeReference(target, *file, (int) tok->getLine(),
                                    (int) tok->getCharPositionInLine());
        } catch (...) {
            // Best-effort: a throw here must never fail a lint or a build.
        }
    }

    // Resolve the construction's static type: the built-in value types first,
    // then the named class (scoped), its instantiation, and any `[]` wrapping.
    void NewExpression::resolveTypes(CajetaModulePtr module) {
        AbstractSyntaxNode::resolveTypes(module);
        if (typeName.empty()) return;
        if (auto vt = resolveVectorNew(module, typeName, typeArguments)) {
            resolvedType = vt;
            return;
        }
        if (auto mt = resolveMatrixNew(module, typeName, typeArguments)) {
            resolvedType = mt;
            return;
        }
        if (auto qt = resolveQuaternionNew(module, typeName, typeArguments)) {
            resolvedType = qt;
            return;
        }
        // boundElementType wins when set: captured with the substitution stack live.
        CajetaTypePtr type = boundElementType;
        // Bare names resolve SCOPED FIRST, as generateCode does: the raw global
        // short-name key is last-writer-wins and shadows with the wrong class.
        if (!type && package.empty()) {
            type = CajetaType::ofScoped(typeName, module);
        }
        if (!type) type = CajetaType::of(typeName, package);
        if (!type) type = CajetaType::ofScoped(typeName, module);
        if (!type) return;
        if (!typeArguments.empty()) {
            auto klass = dynamic_pointer_cast<CajetaClass>(type);
            // Same-short-name collision guard: `heap Stream<int32>()` must build
            // the generic Stream, not a non-generic class of the same short name.
            if (!klass || !klass->isTemplate()) {
                if (auto t = CajetaType::findTemplateByShortName(typeName)) klass = dynamic_pointer_cast<CajetaClass>(t);
            }
            if (klass && klass->isTemplate()) {
                type = klass->instantiate(typeArguments);
            } else {
                throw Exception(
                    "type '" + typeName + "' is not a template but was given "
                    "type arguments", "CAJETA_ERROR_TYPE_ARGS_ON_NON_TEMPLATE");
            }
        } else if (!isDiamond) {
            type = defaultedInstantiation(type, typeName);
        }
        // `new T[N]` has static type T[], not T: wrap once per `[]` pair, or a
        // catch-all load reads sizeof(T) bytes out of the header's size prefix.
        if (auto arr = dynamic_pointer_cast<ArrayCreatorRest>(creatorRest)) {
            int depth = arr->getTotalBracketPairs();
            for (int i = 0; i < depth; i++) {
                type = make_shared<CajetaArray>(module, type);
            }
        }
        // Diamond (`heap Box<>(7)`): infer now so resolvedType is the concrete
        // instantiation — an open template scores as unrelated to `Box<int32>`.
        if (isDiamond) {
            auto klass = dynamic_pointer_cast<CajetaClass>(type);
            if (klass && klass->isTemplate()) {
                if (auto ccr = dynamic_pointer_cast<ClassCreatorRest>(creatorRest)) {
                    vector<CajetaTypePtr> argTypes;
                    bool allResolved = true;
                    for (auto& p : ccr->getParameters()) {
                        if (!p.expression) { allResolved = false; break; }
                        if (!p.expression->getResolvedType()) {
                            p.expression->resolveTypes(module);
                        }
                        auto t = p.expression->getResolvedType();
                        if (!t) { allResolved = false; break; }
                        argTypes.push_back(t);
                    }
                    if (allResolved) {
                        type = klass->instantiate(klass->inferDiamondArgs(argTypes));
                    }
                }
            }
        }
        resolvedType = type;

        // LINT ONLY: `creatorRest` is not in `children`, so nothing else walks its
        // arguments; a build resolves it later, under substitution, in generateCode.
        if (module && module->isResolutionOnly()) {
            if (creatorRest) {
                try { creatorRest->resolveTypes(module); } catch (...) { }
            }
            recordConstructorCallXref(type, module);
        }
    }

    // Recorded here rather than in ClassCreatorRest because this is where the
    // created type is known; the SITE is the creator's, so build and lint merge.
    void NewExpression::recordConstructorCallXref(const CajetaTypePtr& type,
                                                  CajetaModulePtr module) {
        if (!xref::captureEnabled() || !type || !creatorRest) return;
        auto ccr = dynamic_pointer_cast<ClassCreatorRest>(creatorRest);
        if (!ccr) return;
        auto klass = dynamic_pointer_cast<CajetaClass>(type);
        if (!klass) return;

        // A unique arity match only: a wrong edge sends "who constructs this" to
        // the wrong overload. Scanned over getMethods(), NOT getMethodList() —
        // addMethod never pushes a constructor onto methodList.
        const size_t argc = ccr->getParameters().size();
        MethodPtr match;
        for (auto& [_, mm] : klass->getMethods()) {
            if (!mm || !mm->isConstructor()) continue;
            auto pl = mm->getParameterList();
            size_t formalCount = pl.size();
            if (!pl.empty() && pl.front()->getName() == "this") formalCount--;
            if (formalCount != argc) continue;
            if (match) return;
            match = mm;
        }
        if (!match) return;

        xref::CallSiteScope xrefSite(creatorRest->getSourceFile(),
                                     creatorRest->getSourceLine(),
                                     creatorRest->getSourceColumn());
        CajetaClass::noteResolvedCallXref(match, /*isConstructor=*/true, module);
    }

    // Emit the construction: built-in value types build an SSA vector in place;
    // everything else resolves the target type and hands CreatorRest the flags.
    llvm::Value* NewExpression::generateCode(CajetaModulePtr module) {
        if (!creatorRest) {
            return nullptr;
        }
        // `shared` is device-only workgroup placement handled by the kernel
        // lowerer, so reaching the host path means it was written outside a kernel.
        if (sharedAlloc) {
            throw cajeta::Exception(
                "`shared` placement is only valid inside an @Kernel body "
                "(GPU workgroup-shared memory)", "XPU-K03");
        }
        if (auto vecTy = resolveVectorNew(module, typeName, typeArguments)) {
            auto ccr = dynamic_pointer_cast<ClassCreatorRest>(creatorRest);
            if (!ccr) {
                throw Exception(
                    "Vector construction requires a (a, b, ...) argument list",
                    "CAJETA_ERROR_VECTOR_CONSTRUCT");
            }
            unsigned lanes = vecTy->getLanes();
            auto& params = ccr->getParameters();
            if (params.size() != lanes) {
                throw Exception(
                    "Vector<...," + std::to_string(lanes) + "> needs "
                    + std::to_string(lanes) + " arguments (got "
                    + std::to_string(params.size()) + ")",
                    "CAJETA_ERROR_VECTOR_CONSTRUCT");
            }
            llvm::Type* elemTy = vecTy->getElementType()->getLlvmType();
            llvm::IRBuilder<>* b = module->getBuilder();
            std::vector<llvm::Value*> elems;
            elems.reserve(lanes);
            for (auto& p : params) {
                llvm::Value* v = p.expression->generateCode(module);
                v = loadIfLValue(module, v, p.expression);
                elems.push_back(vecops::coerceScalar(*b, v, elemTy));
            }
            return vecops::buildVector(*b, elemTy, lanes, elems);
        }
        if (auto matTy = resolveMatrixNew(module, typeName, typeArguments)) {
            auto ccr = dynamic_pointer_cast<ClassCreatorRest>(creatorRest);
            if (!ccr) {
                throw Exception(
                    "Matrix construction requires a (a, b, ...) argument list",
                    "CAJETA_ERROR_MATRIX_CONSTRUCT");
            }
            unsigned lanes = matTy->getLanes();   // R*C
            auto& params = ccr->getParameters();
            if (params.size() != lanes) {
                throw Exception(
                    "Matrix<...," + std::to_string(matTy->getRows()) + ","
                    + std::to_string(matTy->getCols()) + "> needs "
                    + std::to_string(lanes) + " arguments (got "
                    + std::to_string(params.size()) + ")",
                    "CAJETA_ERROR_MATRIX_CONSTRUCT");
            }
            llvm::Type* elemTy = matTy->getElementType()->getLlvmType();
            llvm::IRBuilder<>* b = module->getBuilder();
            std::vector<llvm::Value*> elems;
            elems.reserve(lanes);
            for (auto& p : params) {
                llvm::Value* v = p.expression->generateCode(module);
                v = loadIfLValue(module, v, p.expression);
                elems.push_back(vecops::coerceScalar(*b, v, elemTy));
            }
            return matops::buildMatrix(*b, elemTy, matTy->getRows(),
                                       matTy->getCols(), elems);
        }
        if (auto qTy = resolveQuaternionNew(module, typeName, typeArguments)) {
            auto ccr = dynamic_pointer_cast<ClassCreatorRest>(creatorRest);
            if (!ccr || ccr->getParameters().size() != 4) {
                throw Exception(
                    "Quaternion<T> needs 4 arguments (w, x, y, z)",
                    "CAJETA_ERROR_QUATERNION_CONSTRUCT");
            }
            llvm::Type* elemTy = qTy->getElementType()->getLlvmType();
            llvm::IRBuilder<>* b = module->getBuilder();
            std::vector<llvm::Value*> elems;
            elems.reserve(4);
            for (auto& p : ccr->getParameters()) {
                llvm::Value* v = loadIfLValue(module,
                    p.expression->generateCode(module), p.expression);
                elems.push_back(vecops::coerceScalar(*b, v, elemTy));
            }
            return vecops::buildVector(*b, elemTy, 4, elems);
        }
        // typeName is the class for `new Foo()`, the element type for `new T[...]`.
        CajetaTypePtr type = boundElementType;
        // Bare names resolve SCOPED FIRST: the raw global short-name key is
        // last-writer-wins, and would hijack `heap Event(...)` inside the stdlib.
        if (!type && package.empty()) {
            type = CajetaType::ofScoped(typeName, module);
        }
        if (!type) type = CajetaType::of(typeName, package);
        // Qualified-miss rescue: the qualified-creator parse can mangle a package
        // (`cajeta.lang.String` arrives as "cajetalang"), so keep it as last tier.
        if (!type) type = CajetaType::ofScoped(typeName, module);
        // Templated `new Box<int32>(...)`: route through the template's
        // instantiation cache so the concrete Box<int32> is what we allocate.
        if (!typeArguments.empty()) {
            auto klass = dynamic_pointer_cast<CajetaClass>(type);
            // Same-short-name collision guard — see resolveTypes above.
            if (!klass || !klass->isTemplate()) {
                if (auto t = CajetaType::findTemplateByShortName(typeName)) klass = dynamic_pointer_cast<CajetaClass>(t);
            }
            if (klass && klass->isTemplate()) {
                type = klass->instantiate(typeArguments);
            } else {
                throw Exception(
                    "type '" + typeName + "' is not a template but was given "
                    "type arguments", "CAJETA_ERROR_TYPE_ARGS_ON_NON_TEMPLATE");
            }
        }
        // Diamond (`new Box<>(args)`): infer from the argument types, which the
        // surrounding method's resolveTypes pass has already resolved.
        else if (isDiamond) {
            auto klass = dynamic_pointer_cast<CajetaClass>(type);
            if (!klass || !klass->isTemplate()) {
                throw Exception(
                    "diamond operator used on non-template type " + typeName,
                    "CAJETA_ERROR_TYPE_INFERENCE");
            }
            auto ccr = dynamic_pointer_cast<ClassCreatorRest>(creatorRest);
            vector<CajetaTypePtr> argTypes;
            if (ccr) {
                for (auto& p : ccr->getParameters()) {
                    if (!p.expression) {
                        throw Exception(
                            "diamond inference: missing argument expression",
                            "CAJETA_ERROR_TYPE_INFERENCE");
                    }
                    if (!p.expression->getResolvedType()) {
                        p.expression->resolveTypes(module);
                    }
                    auto t = p.expression->getResolvedType();
                    if (!t) {
                        throw Exception(
                            "diamond inference: argument type could not be resolved",
                            "CAJETA_ERROR_TYPE_INFERENCE");
                    }
                    argTypes.push_back(t);
                }
            }
            type = klass->instantiate(klass->inferDiamondArgs(argTypes));
        }
        // Bare `heap Box(args)` of a default-bearing template, as resolveTypes did.
        else {
            type = defaultedInstantiation(type, typeName);
        }
        creatorRest->setTargetType(type);
        // Stack placement goes to ClassCreatorRest (alloca over malloc). Array
        // creators ignore it: arrays are always heap-allocated in v1.
        if (stackAlloc) {
            if (auto ccr = dynamic_pointer_cast<ClassCreatorRest>(creatorRest)) {
                ccr->setStackAlloc(true);
            }
        }
        // Arena-eligibility goes to the array creator so the header is bump-
        // allocated. Class instances stay heap: their drop reclaims owned fields.
        if (arenaEligible) {
            if (auto acr = dynamic_pointer_cast<ArrayCreatorRest>(creatorRest)) {
                acr->setArenaEligible(true);
            }
        }
        // NRVO: build straight into the caller's sret slot.
        if (nrvoTarget) {
            creatorRest->setNrvoTarget(nrvoTarget);
        }
        return creatorRest->generateCode(module);
    }
} // code