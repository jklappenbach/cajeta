//
// Created by James Klappenbach on 11/4/22.
//

#pragma once

#include <string>
#include "expression/Expression.h"

using namespace std;

namespace cajeta {

    class Initializer : public AbstractSyntaxNode {
    public:
        virtual ~Initializer() { }
        Initializer(antlr4::Token* token) : AbstractSyntaxNode(token) { }
    };

    typedef shared_ptr<Initializer> InitializerPtr;

    class VariableInitializer : public Initializer {
    public:
        virtual ~VariableInitializer() { }
        VariableInitializer(AbstractSyntaxNodePtr expression, antlr4::Token* token) : Initializer(token) {
            children.push_back(expression);
        }

        llvm::Value* generateCode(CajetaModulePtr module);
    };

    typedef shared_ptr<VariableInitializer> VariableInitializerPtr;

    class ArrayInitializer : public Initializer {
    private:
        list<VariableInitializerPtr> initializers;
    public:
        virtual ~ArrayInitializer() {
            initializers.clear();
        }
        ArrayInitializer(list<InitializerPtr> initializers, antlr4::Token* token) : Initializer(token) {
            children.insert(children.end(), initializers.begin(), initializers.end());
        }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    class VariableDeclarator : public AbstractSyntaxNode {
    private:
        string identifier;
        int arrayDimension;
        InitializerPtr initializer;
        bool reference;
    public:
        VariableDeclarator(string identifier,
            bool reference,
            int arrayDimension,
            InitializerPtr initializer,
            antlr4::Token* token) : AbstractSyntaxNode(token) {
            this->identifier = identifier;
            this->reference = reference;
            this->arrayDimension = arrayDimension;
            this->initializer = initializer;
        }

        string getIdentifier() const {
            return identifier;
        }

        int getArrayDimension() const {
            return arrayDimension;
        }

        InitializerPtr getInitializer() const {
            return initializer;
        }

        bool isReference() const {
            return reference;
        }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    typedef shared_ptr<VariableDeclarator> VariableDeclaratorPtr;
} // code