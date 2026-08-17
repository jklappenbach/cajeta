#include "cajeta/jit/CajetaSymbolIndex.h"

#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"

#include "llvm/IR/Module.h"

#include <algorithm>

#include <cstdlib>
#include <cstdio>

namespace cajeta {

    void CajetaSymbolIndex::addModule(const CajetaModulePtr& module) {
        if (!module) return;
        if (std::find(liveModules.begin(), liveModules.end(), module)
                == liveModules.end()) {
            liveModules.push_back(module);
        }
        for (auto& method : module->getAllMethods()) {
            if (!method) continue;
            const std::string sym = method->getLlvmSymbolName();
            // A method with no symbol name cannot be reached by a JIT lookup,
            // so indexing it would only invite a collision on the empty key.
            if (sym.empty()) continue;
            bySymbol.emplace(sym, method);
        }
        for (auto& [name, klass] : module->getStructures()) {
            if (!klass || !klass->getQName()) continue;
            // Same construction and sanitisation as reflectInvokeFnRef.
            std::string base = "__cajeta_" + klass->getQName()->toCanonical();
            for (char& c : base) {
                if (c == ':' || c == '.' || c == '<' || c == '>' || c == ','
                    || c == ' ') c = '_';
            }
            reflectThunks.emplace(base + "_reflect_invoke",
                                  ReflectThunk{klass, true});
            reflectThunks.emplace(base + "_reflect_new",
                                  ReflectThunk{klass, false});
        }
    }

    MethodPtr CajetaSymbolIndex::find(const std::string& symbol) const {
        auto it = bySymbol.find(symbol);
        return it == bySymbol.end() ? nullptr : it->second;
    }

    const CajetaSymbolIndex::ReflectThunk*
    CajetaSymbolIndex::findReflectThunk(const std::string& symbol) const {
        auto it = reflectThunks.find(symbol);
        return it == reflectThunks.end() ? nullptr : &it->second;
    }

    llvm::GlobalValue* CajetaSymbolIndex::findLiveDefinition(
            const std::string& symbol) const {
        if (symbol.empty()) return nullptr;
        for (auto& m : liveModules) {
            llvm::Module* lm = m ? m->getLlvmModule() : nullptr;
            if (!lm) continue;
            if (llvm::GlobalValue* gv = lm->getNamedValue(symbol)) {
                if (!gv->isDeclaration()) return gv;
            }
        }
        return nullptr;
    }

} // namespace cajeta
