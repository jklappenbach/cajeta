//
// Created by James Klappenbach on 10/4/22.
//

#include "cajeta/type/FormalParameter.h"
#include "cajeta/compile/CajetaModule.h"

namespace cajeta {
    FormalParameter* FormalParameter::fromContext(CajetaParser::FormalParameterContext* ctx, CajetaModule* module) {
        FormalParameter* parameter = nullptr;
        string name = ctx->variableDeclaratorId()->identifier()->getText();
        set<QualifiedName*> annotations;
        set<Modifier> modifiers;
        CajetaParser::TypeTypeContext* ctxType = ctx->typeType();
        CajetaType* type = CajetaType::fromContext(ctxType, module);
        bool reference;

        std::vector<CajetaParser::VariableModifierContext*> variableModifiers = ctx->variableModifier();
        for (auto& ctxVariableModifier: variableModifiers) {
            CajetaParser::AnnotationContext* ctxAnnotation = ctxVariableModifier->annotation();
            if (ctxAnnotation != nullptr) {
                QualifiedName* qName = QualifiedName::fromContext(ctxAnnotation->qualifiedName());
                annotations.insert(qName);
            } else {
                string str = ctxVariableModifier->getText();
                modifiers.insert(Modifiable::toModifier(str));
            }
        }
        if (type != nullptr) {
            parameter = new FormalParameter(name, type, reference, modifiers, annotations);
        }
        return parameter;
    }

    const string& FormalParameter::getName() const {
        return name;
    }

    CajetaType* FormalParameter::getType() const {
        return type;
    }

    string FormalParameter::toCanonical() {
        string canonical = type->toCanonical();
        canonical.append(":").append(name);
        return canonical;
    }

    FormalParameter::FormalParameter(string name, CajetaType* type, bool reference, set<Modifier>& modifiers,
        set<QualifiedName*>& annotations)
        : Field(name, type, reference, modifiers, annotations, nullptr) {
        this->name = name;
        this->type = type;
    }

    Method* FormalParameter::getParent() const {
        return parent;
    }

    void FormalParameter::setParent(Method* parent) {
        this->parent = parent;
    }
}