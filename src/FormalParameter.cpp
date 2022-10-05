//
// Created by James Klappenbach on 10/4/22.
//

#include "cajeta/FormalParameter.h"

namespace cajeta {
    FormalParameter* FormalParameter::fromContext(CajetaParser::FormalParameterContext* ctx) {
        FormalParameter* parameter = nullptr;
        string name = ctx->variableDeclaratorId()->identifier()->getText();
        set<QualifiedName*> annotations;
        set<Modifier> modifiers;
        CajetaParser::TypeTypeContext* ctxType = ctx->typeType();
        Type* type = Type::fromContext(ctxType);

        std::vector<CajetaParser::VariableModifierContext *> variableModifiers = ctx->variableModifier();
        for (auto & ctxVariableModifier : variableModifiers) {
            CajetaParser::AnnotationContext* ctxAnnotation = ctxVariableModifier->annotation();
            if (ctxAnnotation != nullptr) {
                QualifiedName* qName = QualifiedName::toQualifiedName(ctxAnnotation->qualifiedName());
                annotations.insert(qName);
            } else {
                string str = ctxVariableModifier->getText();
                modifiers.insert(Modifiable::toModifier(str));
            }
        }
        if (type != nullptr) {
            parameter = new FormalParameter(name, type, modifiers, annotations);
        }
        return parameter;
    }

    const string& FormalParameter::getName() const {
        return name;
    }

    TypeDefinition* FormalParameter::getTypeDefinition() const {
        return typeDefinition;
    }

    llvm::AllocaInst* FormalParameter::getDefinition() const {
        return definition;
    }

    Type* FormalParameter::getType() const {
        return type;
    }
}