//
// Created by James Klappenbach on 10/2/22.
//

#pragma once

#include "Modifiable.h"
#include "Annotatable.h"
#include "QualifiedName.h"
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

    class CajetaType : public Modifiable, public Annotatable {
    private:
        static map<string, CajetaType*> archive;
    protected:
        bool array;
        QualifiedName* qName;
        llvm::Type* llvmType;
        llvm::Twine canonical;
    public:
        CajetaType() {
            llvmType = nullptr;
        }

        CajetaType(QualifiedName* qName, bool array = false) {
            this->qName = qName;
            this->array = array;
            canonical.concat(qName->toCanonical());
            if (array) {
                canonical.concat("[]");
            }
            archive[qName->toCanonical().str()] = this;
        }

        CajetaType(QualifiedName* qName, llvm::Type* llvmType, bool array = false) {
            this->qName = qName;
            this->llvmType = llvmType;
            this->array = array;
            canonical.concat(qName->toCanonical());
            if (array) {
                canonical.concat("[]");
            }
            archive[qName->toCanonical().str()] = this;
        }

        QualifiedName* getQName() const {
            return qName;
        }

        bool isArray() { return array; }

        virtual llvm::Type* getLlvmType() {
            return llvmType;
        }

        const llvm::Twine& toCanonical() {
            return qName->toCanonical();
        }

        static CajetaType* fromContext(CajetaParser::PrimitiveTypeContext* ctx);
        static CajetaType* fromContext(CajetaParser::TypeTypeOrVoidContext* ctx);
        static CajetaType* fromContext(CajetaParser::TypeTypeContext* ctx);
        static map<string, CajetaType*>& getArchive();
        static void init(llvm::LLVMContext& ctxLlvm);
    };
}