#ifndef TINYLANG_SEMA_SCOPE_H
#define TINYLANG_SEMA_SCOPE_H

#include "tinylang/Basic/LLVM.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace cajeta {

    class Declaration;

    class Scope {
        Scope* Parent;
        StringMap<Declaration*> Symbols;

    public:
        Scope(Scope* Parent = nullptr) : Parent(Parent) {}

        bool insert(Declaration* Declaration);

        Declaration* lookup(StringRef Name);

        Scope* getParent() { return Parent; }
    };
} // namespace cajeta
#endif