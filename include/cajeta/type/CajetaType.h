//
// Created by James Klappenbach on 10/2/22.
//

#pragma once

#include "Modifiable.h"
#include "Annotatable.h"
#include "QualifiedName.h"
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/ADT/StringRef.h>

using namespace std;

namespace cajeta {

    class Method;
    class Field;
    class CajetaModule;

    class CajetaType : public Modifiable, public Annotatable {
    private:
        static map<string, CajetaType*> canonicalMap;
        static map<llvm::Type::TypeID, CajetaType*> typeIdMap;
    protected:
        QualifiedName* qName;
        llvm::Type* llvmType;
        string canonical;
    public:
        CajetaType() {
            llvmType = nullptr;
        }

        CajetaType(QualifiedName* qName) {
            this->qName = qName;
            canonical = qName->toCanonical();
            canonicalMap[canonical] = this;
        }

        CajetaType(QualifiedName* qName, llvm::Type* llvmType) {
            this->qName = qName;
            this->llvmType = llvmType;
            canonical = qName->toCanonical();
            canonicalMap[canonical] = this;
            typeIdMap[llvmType->getTypeID()] = this;
        }

        virtual bool isPrimitive() { return true; }

        QualifiedName* getQName() const {
            return qName;
        }

        virtual llvm::Type* getLlvmType() {
            return llvmType;
        }

        const string& toCanonical() {
            return qName->toCanonical();
        }

        static CajetaType* of(string typeName);
        static CajetaType* of(string typeName, string package);
        static CajetaType* fromContext(CajetaParser::PrimitiveTypeContext* ctx);
        static CajetaType* fromContext(CajetaParser::TypeTypeOrVoidContext* ctx);
        static CajetaType* fromContext(CajetaParser::TypeTypeContext* ctx);
        static map<string, CajetaType*>& getCanonicalMap();
        static map<llvm::Type::TypeID, CajetaType*>& getTypeIdMap();
        static void init(llvm::LLVMContext& ctxLlvm);
        static CajetaType* fromLlvmType(llvm::Type* type, CajetaType* parent = nullptr);
        static CajetaType* fromValue(llvm::Value* value, CajetaType* parent = nullptr);

    };
}