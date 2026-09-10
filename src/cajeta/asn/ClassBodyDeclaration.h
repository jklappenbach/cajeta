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
        // Filled in lockstep with `annotations` by the modifier walk.
        vector<AnnotationInstancePtr> annotationInstances;
    public:
        FieldDeclaration(CajetaTypePtr type, list<VariableDeclaratorPtr> variableDeclarators, antlr4::Token* token)
            : MemberDeclaration(token) {
            this->type = type;
            this->variableDeclarators = variableDeclarators;
        }

        void onModifier(Modifier modifier) override { modifiers.insert(modifier); }

        void addAnnotationInstance(AnnotationInstancePtr inst) {
            if (inst && inst->getName()) annotations.insert(inst->getName());
            annotationInstances.push_back(std::move(inst));
        }

        const vector<AnnotationInstancePtr>& getAnnotationInstances() const {
            return annotationInstances;
        }

        // Append one StructureProperty per declarator to `structure`, carrying the
        // field's modifiers, annotation instances, source position and initializer.
        void updateParent(CajetaClassPtr structure) override {
            int i = structure->getProperties().size();
            for (auto variableDeclarator: variableDeclarators) {
                // The empty annotation set is deliberate: passing `annotations` here
                // as well as looping the instances below would enter each annotation
                // into the property's annotationList twice.
                StructurePropertyPtr property = make_shared<StructureProperty>(
                    variableDeclarator->getIdentifier(),
                    type,
                    modifiers,
                    set<QualifiedNamePtr>(),
                    i++);
                property->setDeclPosition(variableDeclarator->getSourceLine(),
                                          variableDeclarator->getSourceColumn());
                for (auto& inst : annotationInstances) {
                    property->addAnnotationInstance(inst);
                }
                property->setInitializer(variableDeclarator->getInitializer());
                structure->addProperty(property);
            }
        }

        llvm::Value* generateCode(CajetaModulePtr module) override {
            return nullptr;
        }
    };

    // No-op member wrapper for a static-nested class, which has already
    // registered itself in canonicalMap: it keeps the outer's body walk
    // well-typed without contributing fields or methods to the outer.
    class NestedClassDeclaration : public MemberDeclaration {
    private:
        CajetaClassPtr nestedClass;
    public:
        NestedClassDeclaration(CajetaClassPtr cls, antlr4::Token* token)
            : MemberDeclaration(token), nestedClass(cls) { }

        CajetaClassPtr getNestedClass() const { return nestedClass; }

        void onModifier(Modifier) override { }
        void updateParent(CajetaClassPtr) override { /* no-op */ }
        llvm::Value* generateCode(CajetaModulePtr) override { return nullptr; }
    };

    class MethodDeclaration : public MemberDeclaration {
    private:
        MethodPtr method;
    public:
        MethodDeclaration(MethodPtr method, antlr4::Token* token) : MemberDeclaration(token) { this->method = method; }

        MethodPtr getMethod() const { return method; }

        void onModifier(Modifier modifier) override { this->method->addModifier(modifier); }

        void updateParent(CajetaClassPtr structure) override {
            structure->addMethod(method);
        }

        llvm::Value* generateCode(CajetaModulePtr module) override {
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
            return nullptr;
        }
    };
    typedef shared_ptr<ClassBodyDeclaration> ClassBodyDeclarationPtr;
} // code