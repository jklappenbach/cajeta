#include "cajeta/Scope.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringMap.h"

using namespace cajeta;
using namespace llvm;

bool Scope::insert(Declaration* declaration) {
    return symbols.insert(std::pair<StringRef, Declaration*>(declaration->getName(), declaration)).second;
}

Declaration* Scope::lookup(StringRef name) {
    Scope* scope = this;
    while (scope) {
        StringMap<Declaration*>::const_iterator itr = scope->symbols.find(name);
        if (itr != scope->symbols.end()) {
            return itr->second;
        }
        scope = scope->getParent();
    }
    return nullptr;
}
