//
// Created by James Klappenbach on 10/3/22.
//

#pragma once

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
#include <map>
#include "Type.h"

using namespace std;

namespace cajeta {
    class Field;
    class Method;

    class StructType : public Type {
        map<string, Field*> fields;
        map<string, Method*> methods;
        map<string, Field*>& getFields() {
            return fields;
        }

        void setFields(map<string, Field*>& fields) {
            this->fields.merge(fields);
        }

        map<string, Method*>& getMethods() {
            return methods;
        }

        void setMethods(map<string, Method*>& methods) {
            this->methods.merge(methods);
        }

        virtual llvm::Type* getLlvmType(llvm::LLVMContext* context);
    };
}
