//
// Created by James Klappenbach on 10/4/22.
//

#include "FormalParameter.h"
#include "../compile/CajetaModule.h"
#include "../asn/expression/Expression.h"
#include "../asn/AnnotationParser.h"
#include "../error/Exception.h"
#include "../error/Diagnostics.h"
#include "CajetaArray.h"

namespace cajeta {
    FormalParameterPtr FormalParameter::fromContext(CajetaParser::LastFormalParameterContext* ctx, CajetaModulePtr module) {
        if (!ctx) return nullptr;
        string name = ctx->variableDeclaratorId()->identifier()->getText();
        set<QualifiedNamePtr> annotations;
        set<Modifier> modifiers;
        CajetaTypePtr elemType = CajetaType::fromContext(ctx->typeType(), module);
        if (!elemType) {
            // Diagnose rather than return null silently: the caller skips a
            // null parameter, so an unresolved type would otherwise change
            // the method's arity without a word.
            if (ctx->typeType() != nullptr) {
                reportOrThrow(ctx->typeType()->getStart(),
                    "CAJETA_ERROR_UNRESOLVED_TYPE",
                    "unresolved type '" + ctx->typeType()->getText()
                        + "' in varargs parameter '" + name + "'");
            }
            return nullptr;
        }
        // Wrap as T[] — varargs callers will pack trailing args into a
        // fresh array of this element type and pass it as the single
        // value at this parameter slot.
        auto arrType = make_shared<CajetaArray>(module, elemType);
        if (module) {
            module->getStructures()[arrType->toCanonical()] =
                static_pointer_cast<CajetaClass>(arrType);
        }
        return make_shared<FormalParameter>(name,
            static_pointer_cast<CajetaType>(arrType), modifiers, annotations);
    }

    FormalParameterPtr FormalParameter::fromContext(CajetaParser::FormalParameterContext* ctx, CajetaModulePtr module) {
        FormalParameterPtr parameter = nullptr;
        string name = ctx->variableDeclaratorId()->identifier()->getText();
        // Constructed empty — parameter annotations (names AND argument values,
        // REFL-6b) are added below via addAnnotationInstance, which fills the
        // annotationList / annotations set / annotationInstances in lockstep.
        // (Passing names here AND adding instances would double-count, the same
        // trap FieldDeclaration::updateParent hit.)
        set<QualifiedNamePtr> annotations;
        set<Modifier> modifiers;
        vector<AnnotationInstancePtr> paramAnnotations;
        CajetaParser::TypeTypeContext* ctxType = ctx->typeType();
        CajetaTypePtr type = CajetaType::fromContext(ctxType, module);
        if (ctxType != nullptr && !type) {
            // Same rationale as the varargs overload above: a silently
            // dropped parameter mis-arities the method.
            reportOrThrow(ctxType->getStart(),
                "CAJETA_ERROR_UNRESOLVED_TYPE",
                "unresolved type '" + ctxType->getText()
                    + "' in parameter '" + name + "'");
        }

        std::vector<CajetaParser::VariableModifierContext*> variableModifiers = ctx->variableModifier();
        for (auto& ctxVariableModifier: variableModifiers) {
            CajetaParser::AnnotationContext* ctxAnnotation = ctxVariableModifier->annotation();
            if (ctxAnnotation != nullptr) {
                // Capture the full typed instance (was: name only) so the
                // parameter's reflective #AnnotationDesc rows carry arg values.
                if (auto inst = parseAnnotationInstance(ctxAnnotation)) {
                    paramAnnotations.push_back(inst);
                }
            } else {
                string str = ctxVariableModifier->getText();
                modifiers.insert(Modifiable::toModifier(str));
            }
        }
        if (type != nullptr) {
            parameter = make_shared<FormalParameter>(name, type, modifiers, annotations);
            for (auto& inst : paramAnnotations) {
                parameter->addAnnotationInstance(inst);
            }
            // `#T x` — parameter takes ownership at the callsite. The token comes
            // before the type in the grammar (`REFERENCE? typeType`), so checking
            // ctx->REFERENCE() here is enough.
            if (ctx->REFERENCE() != nullptr) {
                parameter->setTransferred(true);
            }
            // title-tracking §8.1 (plan 7.2.1) — the §4.2 dissolve and the
            // borrow-mode gates were retired with owning instantiations: an
            // authored `#T` formal is a hard must-own edge in every
            // instantiation (spec §8.2).
            // Optional default value: `int32 x = 42`. Captured at parse time
            // so the call-site fill-in path can clone the expression's AST
            // node and emit it for each missing argument.
            if (ctx->ASSIGN() && ctx->expression()) {
                parameter->setDefaultValue(Expression::fromContext(ctx->expression()));
            }
        }
        return parameter;
    }

    string FormalParameter::toCanonical(bool labeled) {
        string canonical = Annotatable::toCanonical();
        canonical.append(Modifiable::toCanonical());
        if (labeled) {
            canonical = name + ": ";
        }
        canonical.append(type->toCanonical());
        return canonical;
    }

    FormalParameter::FormalParameter(string name, CajetaTypePtr type, set<Modifier>& modifiers, set<QualifiedNamePtr>& annotations) :
            Modifiable(modifiers), Annotatable(annotations), parent(nullptr) {
        this->name = name;
        this->type = type;
    }

    MethodPtr FormalParameter::getParent() const {
        return parent;
    }

    void FormalParameter::setParent(MethodPtr parent) {
        this->parent = parent;
    }

    string& FormalParameter::getName() {
        return name;
    }

    void FormalParameter::setName(const string& name) {
        this->name = name;
    }

    CajetaTypePtr FormalParameter::getType() const {
        return type;
    }

    void FormalParameter::setType(CajetaTypePtr type) {
        this->type = type;
    }
}