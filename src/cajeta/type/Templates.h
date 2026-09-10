// Template type parameters for class declarations. A template is not a type:
// instantiating it with concrete arguments produces a new, distinct type, and
// prototype generation defers until a reference site triggers instantiate().

#pragma once

#include <list>
#include <string>
#include "QualifiedName.h"

using namespace std;

namespace cajeta {

    // One declared template parameter, `T` or `<T extends Foo & Bar>`.
    struct TypeParameter {
        string name;
        list<QualifiedNamePtr> bounds;   // empty = unbounded

        // A non-type (value) parameter, `<..., uint32 N>`, binding a compile-time
        // constant rather than a type. It has no `bounds`; the arg must be constant.
        bool isNonType = false;
        string nonTypePrimitive;         // declared primitive name when isNonType

        // VESTIGIAL, always false: a `#` on a type parameter now throws instead.
        bool owningRequired = false;

        // Default type argument `<T = float32>`, held as unresolved text because
        // the prescan capture site has no module. Defaults must be TRAILING.
        string defaultType;

        TypeParameter() = default;
        TypeParameter(string name) : name(std::move(name)) {}
    };

}
