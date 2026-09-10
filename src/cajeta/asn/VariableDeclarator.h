// Created by James Klappenbach on 11/4/22.

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

        // Emits the wrapped expression as an r-value, loading through it when it
        // evaluates to an l-value so the surrounding slot store has a value to
        // store. Throws CAJETA_ERROR_UNRESOLVED_EXPRESSION on a `void` initializer.
        llvm::Value* generateCode(CajetaModulePtr module);
    };

    typedef shared_ptr<VariableInitializer> VariableInitializerPtr;

    // Flags a declaration initializer spelled the legacy way, `T x = #v`, so
    // MoveExpression::generateCode can deprecate it. Shared by both declaration
    // builders, and a no-op unless the initializer is exactly `#v`.
    inline void markLegacyTransferAssign(const InitializerPtr& initializer) {
        auto vi = dynamic_pointer_cast<VariableInitializer>(initializer);
        if (!vi || vi->getChildren().empty()) {
            return;
        }
        if (auto mv = dynamic_pointer_cast<MoveExpression>(vi->getChildren()[0])) {
            mv->setLegacyTransferAssign(true);
        }
    }

    class ArrayInitializer : public Initializer {
    private:
        list<VariableInitializerPtr> initializers;
        // Element type for the literal: `{1, 2, 3}` has no declared type of its own,
        // so the surrounding declaration passes it down before codegen.
        CajetaTypePtr elementType;
    public:
        virtual ~ArrayInitializer() {
            initializers.clear();
        }
        ArrayInitializer(list<InitializerPtr> initializers, antlr4::Token* token) : Initializer(token) {
            children.insert(children.end(), initializers.begin(), initializers.end());
        }

        CajetaTypePtr getElementType() const { return elementType; }
        void setElementType(CajetaTypePtr t) { elementType = std::move(t); }

        // Emits the brace initializer as an array populated in source order; returns
        // null unless setElementType was called first, since the literal carries no
        // type. Data braces are retired, so only function-typed elements survive.
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