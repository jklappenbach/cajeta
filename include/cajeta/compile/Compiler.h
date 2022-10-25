//
// Created by James Klappenbach on 10/24/22.
//

#pragma once

#include <iostream>
#include "cajeta/compile/CajetaParserIRListener.h"
#include <llvm/Support/TargetSelect.h>
#include <filesystem>
#include <utility>
#include "antlr4-runtime.h"
#include "CajetaLexer.h"
#include "CajetaParser.h"
#include <string>

namespace cajeta {

    class Compiler {
    private:
        string targetTriple;
        const llvm::Target* target;
        llvm::LLVMContext* context;
        string cpu = "generic";
        string features = "";
        llvm::TargetMachine* targetMachine;
        llvm::TargetOptions opt;
        llvm::Optional<llvm::Reloc::Model> RM;



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
            RM = llvm::Optional<llvm::Reloc::Model>();
            targetMachine = target->createTargetMachine(targetTriple, cpu, features, opt, RM);
            context = new llvm::LLVMContext;
        }
        ~Compiler() {
            delete context;
        }
        void compile(string srcRootPath, string targetRootPath);

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