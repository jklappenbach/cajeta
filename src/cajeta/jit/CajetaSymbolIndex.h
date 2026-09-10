#pragma once

// lazy-codegen Unit 1 (spec 3.3): mangled symbol name -> the method that would be emitted
// under it, so indexing replaces emitting as the JIT's startup cost. The key is
// Method::getLlvmSymbolName(), the same name generateCode() emits under.

#include "cajeta/method/Method.h"

#include <cstddef>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

namespace llvm { class GlobalValue; }

namespace cajeta { class CajetaClass; }

namespace cajeta {

    class CajetaModule;
    using CajetaModulePtr = std::shared_ptr<CajetaModule>;

    class CajetaSymbolIndex {
    public:
        // Index every method of every module. Idempotent and ADDITIVE: codegen defines new
        // methods mid-run, and a destructive rebuild would drop entries the JIT resolved.
        template <typename Range>
        void build(const Range& modules) {
            for (auto& m : modules) addModule(m);
        }

        // Additive re-scan of the modules already handed to build(), called before the
        // generator concedes a miss. Cheap: a stable structure count means nothing is new.
        void refresh();

        // Definitions codegen synthesizes OUTSIDE getAllMethods - drop thunks, vtable and
        // #ClassObject globals - searched live. Returns a definition, never a declaration.
        llvm::GlobalValue* findLiveDefinition(const std::string& symbol) const;

        // Reflective thunks (spec 2.4), emitted by a loop only an eager session runs yet
        // referenced from every heap class's RTTI wiring, so the generator emits on demand.
        struct ReflectThunk {
            std::shared_ptr<CajetaClass> klass;
            bool isInvoke = false;   // else reflect_new
        };
        const ReflectThunk* findReflectThunk(const std::string& symbol) const;

        // The method emitted under `symbol`, or nullptr; an unknown symbol is a plain miss.
        MethodPtr find(const std::string& symbol) const;

        size_t size() const { return bySymbol.size(); }
        void clear() { bySymbol.clear(); }

    private:
        // Records `module` (deduped) and indexes its methods by LLVM symbol plus the
        // two reflect thunks per structure. Additive: emplace keeps the first entry
        // under a symbol, so re-indexing never displaces what the JIT resolved.
        void addModule(const CajetaModulePtr& module);

        std::unordered_map<std::string, MethodPtr> bySymbol;
        std::unordered_map<std::string, ReflectThunk> reflectThunks;
        std::vector<CajetaModulePtr> liveModules;   // deduped, build order
        size_t lastStructureCount = 0;              // refresh() change gate
    };

} // namespace cajeta
