//
// Created by James Klappenbach on 10/4/22.
//

#include "FormalParameter.h"
#include "../compile/CajetaModule.h"

namespace cajeta {
    FormalParameterPtr FormalParameter::fromContext(CajetaParser::FormalParameterContext* ctx, CajetaModulePtr module) {
        FormalParameterPtr parameter = nullptr;
        string name = ctx->variableDeclaratorId()->identifier()->getText();
        set<QualifiedNamePtr> annotations;
        set<Modifier> modifiers;
        CajetaParser::TypeTypeContext* ctxType = ctx->typeType();
        CajetaTypePtr type = CajetaType::fromContext(ctxType, module);

        std::vector<CajetaParser::VariableModifierContext*> variableModifiers = ctx->variableModifier();
        for (auto& ctxVariableModifier: variableModifiers) {
            CajetaParser::AnnotationContext* ctxAnnotation = ctxVariableModifier->annotation();
            if (ctxAnnotation != nullptr) {
                QualifiedNamePtr qName = QualifiedName::fromContext(ctxAnnotation->qualifiedName());
                annotations.insert(qName);
            } else {
                string str = ctxVariableModifier->getText();
                modifiers.insert(Modifiable::toModifier(str));
            }
        }
        if (type != nullptr) {
            parameter = make_shared<FormalParameter>(name, type, modifiers, annotations);
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