//
// Created by James Klappenbach on 2/20/22.
//

#pragma once

#include <string>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <cajeta/ParseContext.h>
#include <cajeta/Generics.h>
#include <CajetaParser.h>

using namespace std;

namespace cajeta {

    struct Field;
    struct Scope;

    struct TypeDefinition {
        string name;
        llvm::Module* module;
        llvm::Type* type;
        bool reference;

        virtual void define() = 0;

        virtual void allocate() = 0;

        virtual void release() = 0;

        TypeDefinition() {
            reference = false;
        }

        TypeDefinition(string name, llvm::Module* module, llvm::Type* type, bool reference) {
            this->name = name;
            this->module = module;
            this->type = type;
            this->reference = reference;
        }

        static TypeDefinition* fromName(string name, ParseContext* ctxParse);
        static TypeDefinition* fromContext(CajetaParser::TypeTypeOrVoidContext* ctxTypeType, ParseContext* ctxParse);
    };

    struct NativeTypeDefinition : TypeDefinition {
        NativeTypeDefinition(string name, llvm::Module* module, llvm::Type* type) :
                NativeTypeDefinition(name, module, type, false) {
        }
        NativeTypeDefinition(string name, llvm::Module* module, llvm::Type* type, bool reference) {
            this->name = name;
            this->module = module;
            this->type = type;
            this->reference = reference;
        }

        virtual void define() {

        }

        virtual void allocate() {

        }

        virtual void release() {

        }

        static TypeDefinition* fromName(string name, ParseContext* ctxParse);
    };

    struct StructureDefinition : TypeDefinition {
        list<TypeParameter*> typeParameters;
        llvm::StructType* structType;
        int accessModifiers;
        string name;
        string package;
        list<string> typeList;  // Inheritance chain
        map<string, list<pair<string, llvm::Type*>>> methods;
        std::map<std::string, Field*> fields;
        llvm::Module* module;

        StructureDefinition() {
            accessModifiers = 0;
        }
        string getCanonicalName() {
            return package + "." + name;
        }
    };

    struct ClassDefinition : StructureDefinition {
        Scope* scope;
        void createBody() {
            // TODO build me!
        }

        virtual void define() { }
        virtual void allocate() { }
        virtual void release() { }
    };

    struct EnumDefinition : StructureDefinition {
        list<string> constants; // Enum specific,
        void createBody() {
            // TODO build me!
        }
    };

    struct InterfaceDefinition : StructureDefinition {
        void createBody() {
            // TODO build me!
        }
    };

    struct RecordDefinition : StructureDefinition {
        void createBody() {
            // TODO build me!
        }
    };
}