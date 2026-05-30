//
// Created by James Klappenbach on 4/19/23.
//

#include "NewExpression.h"
#include "CreatorRest.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaArray.h"
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
    void NewExpression::resolveTypes(CajetaModulePtr module) {
        AbstractSyntaxNode::resolveTypes(module);
        if (typeName.empty()) return;
        // boundElementType wins when set: it was captured at parse
        // time when the template-substitution stack was live, so it
        // already reflects T → concrete-arg even though the stack is
        // long gone by the time resolveTypes runs.
        CajetaTypePtr type = boundElementType;
        if (!type) type = CajetaType::of(typeName, package);
        if (!type) type = CajetaType::of(typeName);
        if (!type) return;
        if (!typeArguments.empty()) {
            auto klass = dynamic_pointer_cast<CajetaClass>(type);
            // Same-short-name collision guard (see CajetaType::
            // findTemplateByShortName): `heap Stream<int32>()` must build the
            // generic cajeta.lang.stream.Stream, not the non-generic
            // cajeta.xpu.core.Stream the bare-name lookup may have landed.
            if (!klass || !klass->isTemplate()) {
                if (auto t = CajetaType::findTemplateByShortName(typeName)) klass = dynamic_pointer_cast<CajetaClass>(t);
            }
            if (klass && klass->isTemplate()) {
                type = klass->instantiate(typeArguments);
            }
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
        // Skip diamond — needs inference that runs at generateCode.
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
        // Look up the target type by name. typeName names the class for `new Foo()`, or
        // the element type for `new T[...]`. Package is "" for primitives (e.g. int32).
        // boundElementType was captured at parse-walk time when the
        // substitution stack was live; prefer it. See NewExpression.h.
        CajetaTypePtr type = boundElementType;
        if (!type) type = CajetaType::of(typeName, package);
        if (!type) {
            // Fallback to canonical lookup by bare typeName for primitives.
            type = CajetaType::of(typeName);
        }
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
        return creatorRest->generateCode(module);
    }
} // code