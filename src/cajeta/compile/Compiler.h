//
// Created by James Klappenbach on 10/24/22.
//

#pragma once

#include <iostream>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Support/Host.h>
#include <llvm/MC/TargetRegistry.h>
#include <filesystem>
#include <utility>
#include "antlr4-runtime.h"
#include "../type/CajetaType.h"
#include "CajetaLexer.h"
#include "CajetaParser.h"
#include <string>
#include "../error/Exception.h"

using namespace std;
namespace cajeta {
    class CajetaModule;

    class AbstractSyntaxNode;

    class Compiler {
    private:
        string targetTriple;
        const llvm::Target* target;
        llvm::LLVMContext llvmContext;
        string cpu = "generic";
        string features = "";
        llvm::TargetMachine* targetMachine;
        llvm::TargetOptions opt;
        std::optional<llvm::Reloc::Model> RM;

    public:
        Compiler() {
            llvm::InitializeAllTargets();
            llvm::InitializeAllTargetMCs();
            llvm::InitializeAllAsmPrinters();
            targetTriple = llvm::sys::getDefaultTargetTriple();
            string error;
            target = llvm::TargetRegistry::lookupTarget(targetTriple, error);

            if (!error.empty()) {
                cerr << "Could not lookup target: " << error;
            }
            targetMachine = target->createTargetMachine(targetTriple, cpu, features, opt, RM);
            CajetaType::init(llvmContext);
        }

        ~Compiler() { }

        void compile(string entryMethod, string sourceRootPath, string archiveRootPath);

        const string& getCpu() const {
            return cpu;
        }

        void setCpu(const string& cpu) {
            this->cpu = cpu;
        }

        const string& getFeatures() const {
            return features;
        }

        void setFeatures(const string& features) {
            this->features = features;
        }
    };
} // cajeta