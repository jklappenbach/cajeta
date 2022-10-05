//
// Created by James Klappenbach on 2/20/22.
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
#include <map>
//#include "Field.h"
//#include "Type.h"
//#include "Method.h"
//#include "QualifiedName.h"

using namespace std;

namespace cajeta {
    struct TypeDefinition;

    struct ParseContext {
        llvm::LLVMContext* llvmContext;
        llvm::Module* module;
        llvm::IRBuilder<>* builder;
        map<string, TypeDefinition*> types;
//        static map<QualifiedName*, Type*> typeAnnotations;
//        static map<QualifiedName*, Field*> fieldAnnotations;
//        static map<QualifiedName*, Method*> methodAnnotations;

        ParseContext(llvm::LLVMContext* llvmContext,
                     llvm::Module* module,
                     llvm::IRBuilder<>* builder) {
            this->llvmContext = llvmContext;
            this->module = module;
            this->builder = builder;
        }
    };
}