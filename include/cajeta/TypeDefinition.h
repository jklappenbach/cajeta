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

    struct Scope;
    struct Method;

    struct TypeDefinition {
        string name;
        llvm::Module* module;
        llvm::Type* type;
        bool reference;
        llvm::StructType* structType;

        virtual void define() = 0;
        virtual void allocate() = 0;
        virtual void release() = 0;

        TypeDefinition() {
            reference = false;
        }

        TypeDefinition(string name, llvm::Module* module) {
            this->name = name;
            this->module = module;
        }

        void createType() {

        }

        void createInstance() {

        }

        void createReference() {

        }

        static TypeDefinition* fromName(string name, ParseContext* ctxParse);
        static TypeDefinition* fromContext(CajetaParser::TypeTypeOrVoidContext* ctxTypeType, ParseContext* ctxParse);
    };

    /**
     *  CHAR                'char'
     *  INT16:              'int16';
     *  UINT16:             'uint16';
     *  INT32:              'int32';
     *  UINT32:             'uint32';
     *  INT64:              'int64';
     *  UINT64:             'uint64';
     *  INT128:             'int128';
     *  UINT128:            'uint128';
     *  FLOAT16:            'float16';
     *  FLOAT32:            'float32';
     *  FLOAT64:            'float64';
     */
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
    };

    struct ClassDefinition : TypeDefinition {
        list<TypeParameter*> typeParameters;
        llvm::StructType* structType;
        list<int> accessModifiers;
        string name;
        string package;
        list<string> typeList;  // Inheritance chain
        map<string,Method*> methods;
        //map<string, Field*> fields;
        llvm::Module* module;

        ClassDefinition(list<int> accessModifiers, string name, string package) {
            this->accessModifiers.insert(accessModifiers.end(), accessModifiers.begin(), accessModifiers.end());
            this->name = name;
            this->package = package;
        }
        string getCanonicalName() {
            return package + "." + name;
        }

        void createBody() {
            // TODO build me!
        }

        virtual void define() { }
        virtual void allocate() { }
        virtual void release() { }
    };

    struct EnumDefinition : TypeDefinition {
        list<string> constants; // Enum specific,
        void createBody() {
            // TODO build me!
        }
    };

    struct InterfaceDefinition : TypeDefinition {
        void createBody() {
            // TODO build me!
        }
    };

    struct RecordDefinition : TypeDefinition {
        void createBody() {
            // TODO build me!
        }
    };
}