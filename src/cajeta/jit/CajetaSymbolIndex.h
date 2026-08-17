#pragma once

// lazy-codegen Unit 1 (spec 3.3) — mangled symbol name -> the method that would
// be emitted under it.
//
// This is the substitution at the heart of the capability: JIT hosts currently
// call generateCode() on every method they know before running any user code
// (12,379 bodies for a cell computing `20 + 42`, 89% of them stdlib). Indexing
// replaces emitting as the startup cost; Unit 2's DefinitionGenerator consults
// this to emit a body when the JIT asks for its symbol.
//
// The key is Method::getLlvmSymbolName() — the same name generateCode() emits
// under, so a lookup that misses here would miss in the JIT too.

#include "cajeta/method/Method.h"

#include <cstddef>
#include <list>
#include <string>
#include <unordered_map>

namespace cajeta {

    class CajetaModule;
    using CajetaModulePtr = std::shared_ptr<CajetaModule>;

    class CajetaSymbolIndex {
    public:
        // Index every method of every module. Idempotent and ADDITIVE:
        // re-building over a grown module set keeps existing entries and adds
        // the new ones. Rebuilds are routine — codegen instantiates templates
        // and so defines methods that did not exist at the first build
        // (spec 3.5), and the JIT may already have resolved through entries a
        // destructive rebuild would drop.
        //
        // Templated because the hosts hand over a vector (`codegenMods()`) and
        // the compiler a list (`getModules()`).
        template <typename Range>
        void build(const Range& modules) {
            for (auto& m : modules) addModule(m);
        }

        // The method emitted under `symbol`, or nullptr. Never throws — an
        // unknown symbol is an ordinary miss that falls through to the JIT's
        // other generators.
        MethodPtr find(const std::string& symbol) const;

        size_t size() const { return bySymbol.size(); }
        void clear() { bySymbol.clear(); }

    private:
        void addModule(const CajetaModulePtr& module);

        std::unordered_map<std::string, MethodPtr> bySymbol;
    };

} // namespace cajeta
