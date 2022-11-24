//
// Created by James Klappenbach on 11/4/22.
//

#pragma once

#include <cajeta/type/CajetaStructure.h>
#include <cajeta/asn/VariableDeclarator.h>
#include <cajeta/compile/CajetaModule.h>

namespace cajeta {

    class MemberDeclaration : public AbstractSyntaxNode {
    public:
        MemberDeclaration(antlr4::Token* token) : AbstractSyntaxNode(token) { }
        virtual void updateParent(CajetaStructure* structure) = 0;
        virtual void onModifier(Modifier modifier) = 0;
    };

    class FieldDeclaration : public MemberDeclaration {
    private:
        CajetaType* type;
        list<VariableDeclarator*> variableDeclarators;
        set<Modifier> modifiers;
        set<QualifiedName*> annotations;
    public:
        FieldDeclaration(CajetaType* type, list<VariableDeclarator*> variableDeclarators, antlr4::Token* token) : MemberDeclaration(token) {
            this->type = type;
            this->variableDeclarators = variableDeclarators;
        }
        void onModifier(Modifier modifier) override { modifiers.insert(modifier); }

        /**
         * string name,
              CajetaType* type,
              int arrayDimension,
              bool reference,
              Initializer* initializer,
              set<Modifier>& modifiers,
              set<QualifiedName*>& annotations
         * @param structure
         */
        void updateParent(CajetaStructure* structure) override {
            int i = structure->getFields().size();
            for (auto variableDeclarator : variableDeclarators) {
                StructureField* field = new StructureField(variableDeclarator->getIdentifier(),
                                         type,
                                         variableDeclarator->getArrayDimension(),
                                         variableDeclarator->isReference(),
                                         variableDeclarator->getInitializer(),
                                         std::move(modifiers),
                                         annotations, i++);
                structure->addField(field);
            }
        }

        llvm::Value* generateCode(CajetaModule* compilationUnit) override {
            return nullptr;
        }
    };

    class MethodDeclaration : public MemberDeclaration {
    private:
        Method* method;
    public:
        MethodDeclaration(Method* method, antlr4::Token* token) : MemberDeclaration(token) { this->method = method; }
        void onModifier(Modifier modifier) override { this->method->addModifier(modifier); }
        void updateParent(CajetaStructure* structure) override {
            structure->getMethods()[method->getName()] = method;
        }
        llvm::Value* generateCode(CajetaModule* compilationUnit) override {
            return nullptr;
        }
    };

    class ClassBodyDeclaration : public AbstractSyntaxNode {
    private:
        list<MemberDeclaration*> declarations;
    public:
        ClassBodyDeclaration(antlr4::Token* token) : AbstractSyntaxNode(token) { }
        list<MemberDeclaration*>& getDeclarations() { return declarations; }
        virtual void generateSignature(CajetaModule* compilationUnit) override {

        }
        llvm::Value* generateCode(CajetaModule* compilationUnit) override {
            return nullptr;
        }
    };
} // cajeta