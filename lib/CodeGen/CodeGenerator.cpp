//===--- CodeGenerator.cpp - Modula-2 Language Code Generator ---*- C++ -*-===//
//
// Part of the M2Lang Project, under the Apache License v2.0 with
// LLVM Exceptions. See LICENSE file for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the code generator implementation.
///
//===----------------------------------------------------------------------===//

#include "CodeGen/CodeGenerator.h"
#include "CodeGen/CGModule.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

using namespace cajeta;

CodeGenerator* CodeGenerator::create(llvm::LLVMContext& ctxLlvm, ASTContext& ctxAst, llvm::TargetMachine* targetMachine) {
    return new CodeGenerator(ctxLlvm, ctxAst, targetMachine);
}

std::unique_ptr<llvm::Module> CodeGenerator::run(ModuleDeclaration* moduleDeclaration, std::string fileName) {
    std::unique_ptr<llvm::Module> module = std::make_unique<llvm::Module>(fileName, ctx);
    M->setTargetTriple(targetMachine->getTargetTriple().getTriple());
    M->setDataLayout(targetMachine->createDataLayout());
    CGModule cgModule(ctxAst, module.get());
    cgModule.run(Mod);
    return module;
}
