//
// Created by James Klappenbach on 11/4/22.
//

#pragma once

#include <string>
#include <cajeta/asn/Expression.h>

using namespace std;

namespace cajeta {

    class Initializer : public AbstractSyntaxNode {
    public:
        Initializer(antlr4::Token* token) : AbstractSyntaxNode(token) { }
    };

    class VariableInitializer : public Initializer {
    public:
        VariableInitializer(Expression* expression, antlr4::Token* token) : Initializer(token) {
            children.push_back(expression);
        }
        llvm::Value* generateCode(CajetaModule* module) override {
            return children.back()->generateCode(module);
        }
    };

    class ArrayInitializer : public Initializer {
    private:
        list<VariableInitializer*> initializers;
    public:
        ArrayInitializer(list<VariableInitializer*> initializers, antlr4::Token* token) : Initializer(token) {
            children.insert(children.end(), initializers.begin(), initializers.end());
        }

        llvm::Value* generateCode(CajetaModule* module) override {
            for (auto &node : children) {
                node->generateCode(module);
            }
            return nullptr; //llvm::ConstantStruct::get
        }
    };

    class VariableDeclarator : public AbstractSyntaxNode {
    private:
        string identifier;
        int arrayDimension;
        Initializer* initializer;
        bool reference;
    public:
        VariableDeclarator(string identifier,
                           bool reference,
                           int arrayDimension,
                           Initializer* initializer,
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

        Initializer* getInitializer() const {
            return initializer;
        }

        bool isReference() const {
            return reference;
        }

        llvm::Value* generateCode(CajetaModule* module) override;
    };
} // cajeta