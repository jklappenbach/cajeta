//
// Created by James Klappenbach on 10/4/22.
//

#include "FormalParameter.h"
#include "../compile/CajetaModule.h"
#include "CajetaArray.h"

namespace cajeta {
    FormalParameterPtr FormalParameter::fromContext(CajetaParser::LastFormalParameterContext* ctx, CajetaModulePtr module) {
        if (!ctx) return nullptr;
        string name = ctx->variableDeclaratorId()->identifier()->getText();
        set<QualifiedNamePtr> annotations;
        set<Modifier> modifiers;
        CajetaTypePtr elemType = CajetaType::fromContext(ctx->typeType(), module);
        if (!elemType) return nullptr;
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
            // `#T x` — parameter takes ownership at the callsite. The token comes
            // before the type in the grammar (`REFERENCE? typeType`), so checking
            // ctx->REFERENCE() here is enough.
            if (ctx->REFERENCE() != nullptr) {
                parameter->setTransferred(true);
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