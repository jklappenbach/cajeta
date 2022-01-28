#pragma once

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "Declaration.h"

using namespace llvm;

namespace cajeta {
    class Scope {
        Scope* parent;
        StringMap<Declaration*> symbols;
    public:
        Scope(Scope* parent = nullptr) : parent(parent) {}

        bool insert(Declaration* declaration);

        Declaration* lookup(StringRef name);

        Scope* getParent() {
            return parent;
        }
    };
} // namespace cajeta