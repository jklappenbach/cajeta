//
// Created by James Klappenbach on 11/4/22.
//

#pragma once

#include "../type/CajetaClass.h"
#include "VariableDeclarator.h"
#include "../compile/CajetaModule.h"

namespace cajeta {

    class MemberDeclaration : public AbstractSyntaxNode {
    public:
        MemberDeclaration(antlr4::Token* token) : AbstractSyntaxNode(token) { }

        virtual void updateParent(CajetaClassPtr structure) = 0;

        virtual void onModifier(Modifier modifier) = 0;
    };
    typedef shared_ptr<MemberDeclaration> MemberDeclarationPtr;

    class FieldDeclaration : public MemberDeclaration {
    private:
        CajetaTypePtr type;
        list<VariableDeclaratorPtr> variableDeclarators;
        set<Modifier> modifiers;
        set<QualifiedNamePtr> annotations;
    public:
        FieldDeclaration(CajetaTypePtr type, list<VariableDeclaratorPtr> variableDeclarators, antlr4::Token* token)
            : MemberDeclaration(token) {
            this->type = type;
            this->variableDeclarators = variableDeclarators;
        }

        void onModifier(Modifier modifier) override { modifiers.insert(modifier); }

        /**
         * string name,
              CajetaTypePtr type,
              int arrayDimension,
              bool reference,
              Initializer* initializer,
              set<Modifier>& modifiers,
              set<QualifiedName*>& annotations
         * @param structure
         */
        void updateParent(CajetaClassPtr structure) override {
            int i = structure->getProperties().size();
            for (auto variableDeclarator: variableDeclarators) {
                StructurePropertyPtr property = make_shared<StructureProperty>(
                    variableDeclarator->getIdentifier(),
                    type,
                    modifiers,
                    annotations,
                    i++);
                structure->addProperty(property);
            }
        }

        llvm::Value* generateCode(CajetaModulePtr module) override {
            module->getAsnStack().push_back(shared_from_this());
            module->getAsnStack().pop_back();
            return nullptr;
        }
    };

    class MethodDeclaration : public MemberDeclaration {
    private:
        MethodPtr method;
    public:
        MethodDeclaration(MethodPtr method, antlr4::Token* token) : MemberDeclaration(token) { this->method = method; }

        void onModifier(Modifier modifier) override { this->method->addModifier(modifier); }

        void updateParent(CajetaClassPtr structure) override {
            structure->addMethod(method);
        }

        llvm::Value* generateCode(CajetaModulePtr module) override {
            module->getAsnStack().push_back(shared_from_this());
            module->getAsnStack().pop_back();
            return nullptr;
        }
    };

    class ClassBodyDeclaration : public AbstractSyntaxNode {
    private:
        list<MemberDeclarationPtr> declarations;
    public:
        ClassBodyDeclaration(antlr4::Token* token) : AbstractSyntaxNode(token) { }

        list<MemberDeclarationPtr>& getDeclarations() { return declarations; }

        virtual void generateSignature(CajetaModulePtr compilationUnit) override {

        }

        llvm::Value* generateCode(CajetaModulePtr module) override {
            module->getAsnStack().push_back(shared_from_this());
            module->getAsnStack().pop_back();
            return nullptr;
        }
    };
    typedef shared_ptr<ClassBodyDeclaration> ClassBodyDeclarationPtr;
} // code