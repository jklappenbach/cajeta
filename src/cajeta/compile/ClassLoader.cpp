//
// Created by James Klappenbach on 2/10/23.
//

#include "ClassLoader.h"

#include <memory>

namespace cajeta {
    /**
     *
     * @param package
     * @param context
     * @return
     */
    std::unique_ptr<llvm::Module> ClassLoader::loadModules(std::string package, llvm::LLVMContext* context) {
        for (auto &strPath : classPaths) {

        }
        return std::make_unique<llvm::Module>(llvm::StringRef("Null"), *context);
    }
    std::unique_ptr<CajetaStructure> ClassLoader::loadStructure(std::string canonical, llvm::LLVMContext* context) {
        CajetaModulePtr ptr;// Load Module
        return std::make_unique<CajetaStructure>(ptr);
    }

} // cajeta