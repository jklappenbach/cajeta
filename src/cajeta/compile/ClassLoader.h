//
// Created by James Klappenbach on 2/10/23.
//

#pragma once

#include <memory>
#include <regex>
#include <llvm/IRReader/IRReader.h>
#include <cajeta/type/CajetaClass.h>
#include <llvm/Bitcode/BitcodeReader.h>

using namespace std;

namespace cajeta {
    class ClassLoader {
    protected:
        list<string> classPaths;
        // map<string,
    public:
        ClassLoader(string classPath, llvm::LLVMContext* context) {
            if (classPath.empty()) {
                classPath = std::getenv("CAJETA_CLASSPATH");
            }
            if (classPath.empty()) {
                std::filesystem::path cwd = std::filesystem::current_path();
                classPath = cwd.string();
            }
            std::regex regex("\\:");
            std::sregex_token_iterator itr(classPath.begin(), classPath.end(), regex, -1);
            std::sregex_token_iterator end;
            while (itr != end) {
                classPaths.push_back(*itr);
                loadModules(*itr, context);
                ++itr;
            };
        }

        /**
         * If
         * @param package
         * @param context
         * @return
         */
        unique_ptr<llvm::Module> loadModules(std::string package, llvm::LLVMContext* context);

        /**
         *
         * @param canonical
         * @param context
         * @return
         */
        unique_ptr<CajetaClass> loadStructure(std::string canonical, llvm::LLVMContext* context);
    };
} // code