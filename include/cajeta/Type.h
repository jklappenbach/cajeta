//
// Created by James Klappenbach on 10/2/22.
//

#pragma once

#include "Modifiable.h"
#include "QualifiedName.h"
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

    class Type : public Modifiable, public Annotatable {
    private:
        static map<QualifiedName*, Type*> global;
    protected:
        QualifiedName* qName;
        llvm::Type* llvmType;
    public:
        Type() {
            llvmType = nullptr;
        }
        Type(QualifiedName* qName, set<Modifier>& modifiers) :
                Modifiable(modifiers) {
            this->qName = qName;
            global[qName] = this;
            llvmType = nullptr;
        }
        Type(QualifiedName* qName, set<Modifier>& modifiers, set<QualifiedName*>& annotations) :
                Modifiable(modifiers), Annotatable(annotations) {
            this->qName = qName;
            global[qName] = this;
            llvmType = nullptr;
        }
        const QualifiedName* getQName() const {
            return qName;
        }

        virtual llvm::Type* getLlvmType(llvm::LLVMContext* context);

        static cajeta::Type* fromContext(CajetaParser::TypeTypeContext* ctx);
    };
}