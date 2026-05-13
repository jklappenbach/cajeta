//
// Created by James Klappenbach on 4/19/23.
//

#include "NewExpression.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/error/Exception.h"

namespace cajeta {
    llvm::Value* NewExpression::generateCode(CajetaModulePtr module) {
        if (!creatorRest) {
            return nullptr;
        }
        // Look up the target type by name. typeName names the class for `new Foo()`, or
        // the element type for `new T[...]`. Package is "" for primitives (e.g. int32).
        CajetaTypePtr type = CajetaType::of(typeName, package);
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
        return creatorRest->generateCode(module);
    }
} // code