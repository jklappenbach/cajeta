#include "cajeta/compile/DropBackfill.h"

#include <cstdlib>
#include <iostream>
#include <map>

#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"

#include "cajeta/type/CajetaClass.h"

namespace cajeta {

    namespace {
        // symbol → owning class, both families, for every concrete class in
        // the canonical map. Shared by the backfill scan and the merge pin.
        std::map<std::string, CajetaClassPtr> buildDropSymbolMap() {
            std::map<std::string, CajetaClassPtr> classByDropSymbol;
            for (auto& [canon, type] : CajetaType::getCanonicalMap()) {
                auto klass = std::dynamic_pointer_cast<CajetaClass>(type);
                if (!klass || klass->isTemplate()) continue;
                classByDropSymbol[dropSymbolName(canon, /*stack=*/true)] = klass;
                classByDropSymbol[dropSymbolName(canon, /*stack=*/false)] = klass;
            }
            return classByDropSymbol;
        }
    }

    std::string dropSymbolName(const std::string& canonicalTypeName, bool stack) {
        std::string mangled = canonicalTypeName;
        for (char& c : mangled) {
            if (c == ':' || c == '.' || c == '<' || c == '>'
                || c == ',' || c == ' ') {
                c = '_';
            }
        }
        return (stack ? "__cajeta_stack_" : "__cajeta_") + mangled + "_drop";
    }

    void backfillDropFunctions(const std::vector<CajetaModulePtr>& modulesToScan) {
        auto classByDropSymbol = buildDropSymbolMap();
        for (auto& module : modulesToScan) {
            for (auto& fn : module->getLlvmModule()->functions()) {
                if (!fn.isDeclaration()) continue;
                auto hit = classByDropSymbol.find(fn.getName().str());
                if (hit == classByDropSymbol.end()) continue;
                if (std::getenv("CAJETA_DROP_BACKFILL_LOG")) {
                    std::cerr << "cajeta: drop-backfill " << fn.getName().str()
                              << " (declared in "
                              << module->getLlvmModule()->getName().str()
                              << ")\n";
                }
                // Both families, whichever was declared — the historical AOT
                // behavior; the sibling wrapper is cheap and often needed next.
                hit->second->getOrCreateStackDropFunction();
                hit->second->getOrCreateDropFunction();
            }
        }
    }

    void pinDropFunctionDefinitions(const std::vector<CajetaModulePtr>& modules) {
        auto classByDropSymbol = buildDropSymbolMap();
        for (auto& module : modules) {
            for (auto& fn : module->getLlvmModule()->functions()) {
                if (fn.isDeclaration()) continue;
                if (!fn.hasLinkOnceODRLinkage()) continue;
                if (classByDropSymbol.find(fn.getName().str())
                    == classByDropSymbol.end()) continue;
                if (std::getenv("CAJETA_DROP_BACKFILL_LOG")) {
                    std::cerr << "cajeta: drop-pin " << fn.getName().str()
                              << " (defined in "
                              << module->getLlvmModule()->getName().str()
                              << ")\n";
                }
                fn.setLinkage(llvm::GlobalValue::WeakODRLinkage);
                fn.setComdat(nullptr);
            }
        }
    }

} // namespace cajeta
