//
// Created by James Klappenbach on 10/2/22.
//

#pragma once

#include "Modifiable.h"
#include "cajeta/module/QualifiedName.h"
#include "Annotatable.h"
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
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/ADT/StringRef.h>

using namespace std;

namespace cajeta {

    class Method;
    class Field;
    class CompilationUnit;

    class CajetaType {
    protected:
        QualifiedName* qName;
        llvm::Type* llvmType;
    public:
        static map<QualifiedName*, CajetaType*> types;

        CajetaType() {
            llvmType = nullptr;
        }

        CajetaType(QualifiedName* qName) {
            this->qName = qName;
            types[qName] = this;
        }

        CajetaType(QualifiedName* qName, llvm::Type* llvmType) {
            this->qName = qName;
            this->llvmType = llvmType;
            types[qName] = this;
        }

        QualifiedName* getQName() const {
            return qName;
        }


        virtual llvm::Type* getLlvmType() {
            return llvmType;
        }
        static CajetaType* fromContext(CajetaParser::TypeTypeContext* ctx);
        static void init(llvm::LLVMContext& ctxLlvm);
    };
}