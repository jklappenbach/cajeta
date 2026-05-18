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
        // Typed annotation captures (A8). Populated by
        // visitClassBodyDeclaration's modifier walk in lockstep with
        // `annotations`. Each entry is propagated to every
        // StructureProperty produced by updateParent so DI's
        // resolveDependencyGraph can inspect @Inject(name=...,
        // allocate=...) per field.
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

        void updateParent(CajetaClassPtr structure) override {
            int i = structure->getProperties().size();
            for (auto variableDeclarator: variableDeclarators) {
                StructurePropertyPtr property = make_shared<StructureProperty>(
                    variableDeclarator->getIdentifier(),
                    type,
                    modifiers,
                    annotations,
                    i++);
                for (auto& inst : annotationInstances) {
                    property->addAnnotationInstance(inst);
                }
                // Thread the per-declarator initializer through so static
                // fields can fold a literal `= value` into the LLVM
                // global's initializer at class-build time.
                property->setInitializer(variableDeclarator->getInitializer());
                structure->addProperty(property);
            }
        }

        llvm::Value* generateCode(CajetaModulePtr module) override {
            return nullptr;
        }
    };

    // Nested-class wrapper. When the parser encounters
    // `class Outer { public static class Inner { ... } }`, the inner
    // class registers itself into canonicalMap via the recursive
    // visitClassDeclaration call. The outer's class-body iteration
    // would otherwise try to cast the inner's return into a
    // MemberDeclarationPtr and fail; this no-op wrapper keeps the
    // outer's body walk well-typed without contributing fields or
    // methods to the outer (nested classes are independent types).
    //
    // Currently only static-nested classes are supported (no implicit
    // outer-this reference). Holding the CajetaClassPtr keeps a
    // strong reference even if the canonicalMap shape changes.
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