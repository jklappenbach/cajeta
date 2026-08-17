#include "cajeta/jit/CajetaSymbolIndex.h"

#include "cajeta/compile/CajetaModule.h"

#include <cstdlib>
#include <cstdio>

namespace cajeta {

    void CajetaSymbolIndex::addModule(const CajetaModulePtr& module) {
        if (!module) return;
        for (auto& method : module->getAllMethods()) {
            if (!method) continue;
            const std::string sym = method->getLlvmSymbolName();
            // A method with no symbol name cannot be reached by a JIT lookup,
            // so indexing it would only invite a collision on the empty key.
            if (sym.empty()) continue;
            bySymbol.emplace(sym, method);
        }
    }

    MethodPtr CajetaSymbolIndex::find(const std::string& symbol) const {
        auto it = bySymbol.find(symbol);
        return it == bySymbol.end() ? nullptr : it->second;
    }

} // namespace cajeta
