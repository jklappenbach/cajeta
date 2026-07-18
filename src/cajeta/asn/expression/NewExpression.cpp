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
    // Set resolvedType to the target class so call-site overload
    // resolution sees the right type when a `new T(...)` flows in as
    // a method-call argument or constructor argument. Previously this
    // wasn't set, so MethodCallExpression's fallback path (CajetaType::
    // of(value)) inferred the generic `pointer` type — Method::resolveMethod
    // then built the wrong signature ("consume(pointer)" instead of
    // "consume(test.Payload)"), missed the lookup, and invokeMethod
    // returned null. ReturnStatement::generateCode then null-deref'd
    // val->getType() and segfaulted. See AsyncStatus.md § Known gaps
    // before this commit. Diamond inference is deferred to codegen
    // time (it needs each arg's resolvedType, which the surrounding
    // method's resolve-pass populates first); for now, resolvedType
    // stays null on diamond forms until generateCode fills it in.
    // Built-in Vector<T,N>: resolve to the CajetaVector value type. Shared by
    // resolveTypes and generateCode. typeArguments were captured at parse time
    // (the element CajetaType + a CajetaConstantType lane count).
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

    // Built-in Matrix<T,R,C>: resolve to the flat CajetaMatrix value type (B1).
    // Mirrors resolveVectorNew; typeArguments were captured at parse time (the
    // element CajetaType + two CajetaConstantType dimensions).
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

    // Built-in Quaternion<T>: resolve to the flat CajetaQuaternion value type.
    static CajetaQuaternionPtr resolveQuaternionNew(
            CajetaModulePtr module, const string& typeName,
            const vector<CajetaTypePtr>& typeArguments) {
        if (typeName != "Quaternion" || typeArguments.size() != 1) return nullptr;
        return CajetaQuaternion::validateAndCreate(module, typeArguments[0]);
    }

    // Bare construction of a default-bearing template — `heap Box(args)` for
    // `class Box<T = int32>` → Box<int32>. The construction-site twin of
    // CajetaType::fromContext's default resolution (which handles type-use
    // sites: field/param/local/return types), so `heap Texture2D(w, h)` keeps
    // working once Texture2D gains a defaulted parameter. Returns `type`
    // unchanged unless it names a template whose parameters are ALL defaulted.
    // Only the no-type-arguments, non-diamond case calls this; explicit args
    // and `<>` are handled by their own paths.
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
        if (!file) return;   // synthesized source (template/mock): no position
        try {
            // Resolve the created type NAME to its declaration's canonical FQN,
            // scoped like resolveTypes (own package → imports → global) and
            // tolerant of a forward reference — the whole-root export parses in
            // directory order and routinely reaches `heap Square(...)` before
            // Square.cajeta is built. A qualified `heap pkg.Point()` carries a
            // (dot-less-concatenated) package; try it first, else resolve the
            // leaf name scoped. A miss records nothing.
            CajetaModulePtr module = CajetaModule::getActiveModule();
            std::string target;
            if (!package.empty()) {
                std::string qualified = package + "." + typeName;
                if (CajetaType::getArchive().count(qualified)) target = qualified;
            }
            if (target.empty())
                target = CajetaType::canonicalNameScoped(typeName, module);
            if (target.empty()) return;
            // An instantiation (`ArrayList<int32>`) navigates to the TEMPLATE's
            // declaration — strip any argument list, mirroring fromContext.
            auto lt = target.find('<');
            if (lt != std::string::npos) target = target.substr(0, lt);
            xref::noteTypeReference(target, *file, (int) tok->getLine(),
                                    (int) tok->getCharPositionInLine());
        } catch (...) {
            // Reference capture is best-effort: a resolution that throws here
            // (it would throw again, reported, in the codegen pass) must never
            // turn into a lint/compile failure.
        }
    }

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
        // boundElementType wins when set: it was captured at parse
        // time when the template-substitution stack was live, so it
        // already reflects T → concrete-arg even though the stack is
        // long gone by the time resolveTypes runs.
        CajetaTypePtr type = boundElementType;
        if (!type) type = CajetaType::of(typeName, package);
        // Bare name: resolve scoped (own package → imports → global) —
        // the raw global short-name key is last-writer-wins across
        // packages, so `heap Foo()` would build a same-named class from
        // another package whenever one registered later.
        if (!type) type = CajetaType::ofScoped(typeName, module);
        if (!type) return;
        if (!typeArguments.empty()) {
            auto klass = dynamic_pointer_cast<CajetaClass>(type);
            // Same-short-name collision guard (see CajetaType::
            // findTemplateByShortName): `heap Stream<int32>()` must build the
            // generic cajeta.lang.stream.Stream, not the non-generic
            // cajeta.xpu.KernelStream the bare-name lookup may have landed.
            if (!klass || !klass->isTemplate()) {
                if (auto t = CajetaType::findTemplateByShortName(typeName)) klass = dynamic_pointer_cast<CajetaClass>(t);
            }
            if (klass && klass->isTemplate()) {
                type = klass->instantiate(typeArguments);
            }
        } else if (!isDiamond) {
            // Bare `heap Box(args)` of a default-bearing template → Box<defaults>.
            type = defaultedInstantiation(type, typeName);
        }
        // For `new T[N]` / `new T[N][M]`, the value's static type is T[],
        // not T. Wrap in CajetaArray for each `[]` pair so consumers
        // (loadIfLValue's catch-all, assignment slot-type computation,
        // overload resolution) see the array type instead of the element
        // type. Without this, the heap pointer returned by the array
        // creator gets treated as a pointer-to-element, and any catch-
        // all "load from this pointer" code reads sizeof(T) bytes from
        // the size prefix of the header.
        if (auto arr = dynamic_pointer_cast<ArrayCreatorRest>(creatorRest)) {
            int depth = arr->getTotalBracketPairs();
            for (int i = 0; i < depth; i++) {
                type = make_shared<CajetaArray>(module, type);
            }
        }
        // Diamond form (`heap Box<>(7)`): infer the type arguments now so
        // `resolvedType` is the concrete instantiation (`Box<int32>`), not the
        // open template (`Box`). Earlier this was deferred to generateCode,
        // which left resolvedType as the open template through the whole
        // resolve pass — so member access off the result (`heap Box<>(7).id`)
        // and the local-init assignability oracle saw an uninstantiated type
        // and scored it unrelated to the declared `Box<int32>`. The inference
        // reads each constructor-arg's resolved type (available now: the base
        // resolveTypes above walked our children) and is pure inspection —
        // inferDiamondArgs re-parses the template snippet without emitting IR.
        // generateCode runs the same inference independently for codegen; both
        // land on the same instantiation, so they stay in agreement.
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
                    // Only commit the instantiation when every arg type is
                    // known; otherwise leave the open template and let
                    // generateCode's (later, fuller) pass do the inference.
                    if (allResolved) {
                        type = klass->instantiate(klass->inferDiamondArgs(argTypes));
                    }
                }
            }
        }
        resolvedType = type;
    }

    llvm::Value* NewExpression::generateCode(CajetaModulePtr module) {
        if (!creatorRest) {
            return nullptr;
        }
        // `shared` is GPU workgroup-shared placement (NV addrspace 3). It is
        // device-only — the NVPTX kernel lowerer handles it by walking the AST
        // directly (kernels get empty host stubs, so this normally never runs).
        // If it surfaces on the host path, the user wrote `shared` outside a
        // kernel: reject with a clear diagnostic rather than mis-lowering it.
        if (sharedAlloc) {
            throw cajeta::Exception(
                "`shared` placement is only valid inside an @Kernel body "
                "(GPU workgroup-shared memory)", "XPU-K03");
        }
        // Built-in Vector<T,N> construction -> SSA `<N x T>`, no alloc/heap.
        // The `new`/`stack` keyword is purely syntactic here; the result is a
        // register value (a value type, like a primitive).
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
        // Built-in Matrix<T,R,C> construction -> SSA `<R*C x T>` (row-major), no
        // alloc/heap. Like Vector, `new`/`stack` is purely syntactic; R*C scalar
        // arguments fill the matrix row by row.
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
        // Built-in Quaternion<T> construction -> SSA `<4 x T>` (w, x, y, z). Four
        // scalar arguments; `new`/`stack` is purely syntactic (a value type).
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
        // Look up the target type by name. typeName names the class for `new Foo()`, or
        // the element type for `new T[...]`. Package is "" for primitives (e.g. int32).
        // boundElementType was captured at parse-walk time when the
        // substitution stack was live; prefer it. See NewExpression.h.
        CajetaTypePtr type = boundElementType;
        // Bare names resolve SCOPED FIRST (own package → imports → global):
        // `of(name, "")` hits the raw global short-name key, which is
        // last-writer-wins across packages — a user class named `Event`
        // would hijack `heap Event(...)` inside cajeta.xpu.Event.create()
        // during lazy stdlib codegen. ofScoped's tier 3 is that same global
        // key, so this is strictly more precise. Qualified names keep the
        // direct canonical lookup. (Subsumes main f086c73e's scoped
        // fallback — same shadowing fix, stronger ordering.)
        if (!type && package.empty()) {
            type = CajetaType::ofScoped(typeName, module);
        }
        if (!type) type = CajetaType::of(typeName, package);
        // Qualified-miss rescue (pre-existing): the qualified-creator parse
        // can mangle the package (`cajeta.lang.String` arrives as package
        // "cajetalang"), which the legacy global short-name fallback silently
        // absorbed. Keep that rescue as the last tier.
        if (!type) type = CajetaType::ofScoped(typeName, module);
        // Templated `new Box<int32>(...)`: typeArguments were resolved at
        // parse time (in our constructor). Route through the template's
        // instantiation cache so the concrete `Box<int32>` is what we
        // allocate against.
        if (!typeArguments.empty()) {
            auto klass = dynamic_pointer_cast<CajetaClass>(type);
            // Same-short-name collision guard — see resolveTypes above.
            if (!klass || !klass->isTemplate()) {
                if (auto t = CajetaType::findTemplateByShortName(typeName)) klass = dynamic_pointer_cast<CajetaClass>(t);
            }
            if (klass && klass->isTemplate()) {
                type = klass->instantiate(typeArguments);
            }
        }
        // Diamond form (`new Box<>(args)`): infer type arguments from the
        // constructor-call argument types, then route through instantiate.
        // Inference reads each arg expression's already-resolved type — by
        // the time we're in NewExpression::generateCode, the surrounding
        // method has run its resolveTypes pass so child expressions know
        // their types.
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
        // Bare `heap Box(args)` of a default-bearing template → Box<defaults>
        // (mirrors resolveTypes; keeps the codegen-time type identical to the
        // resolve-time one so both agree on the instantiation).
        else {
            type = defaultedInstantiation(type, typeName);
        }
        creatorRest->setTargetType(type);
        // P2a: propagate stack-alloc choice down to ClassCreatorRest so
        // it picks alloca over malloc. Array creators ignore the flag —
        // arrays are always heap-allocated in v1 regardless of allocation
        // prefix (no `stack int32[10]` syntax has user-facing semantics
        // yet; would land as a future feature if needed).
        if (stackAlloc) {
            if (auto ccr = dynamic_pointer_cast<ClassCreatorRest>(creatorRest)) {
                ccr->setStackAlloc(true);
            }
        }
        // U3: propagate arena-eligibility to the array creator so the header is
        // bump-allocated from the frame arena. Only array creators are arena-routed
        // in this unit (class instances stay heap — their drop reclaims owned
        // fields / runs user destructors; see frame-arena-plan 3.2.3).
        if (arenaEligible) {
            if (auto acr = dynamic_pointer_cast<ArrayCreatorRest>(creatorRest)) {
                acr->setArenaEligible(true);
            }
        }
        // NRVO: when this construction is the returned value of a value-
        // returning method, build straight into the caller's sret slot.
        if (nrvoTarget) {
            creatorRest->setNrvoTarget(nrvoTarget);
        }
        return creatorRest->generateCode(module);
    }
} // code