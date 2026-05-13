//
// Created by James Klappenbach on 2/19/22.
//
// Template type parameters for class declarations. The language follows C++'s
// model: a template is not a type — instantiating with concrete type arguments
// produces a new, distinct type. Native/primitive types are valid arguments
// (the C# rule, not the Java rule).
//
// A CajetaClass with non-empty `typeParameters` and empty `typeArguments` is a
// template; its prototype generation is deferred until a reference site like
// `Box<int32>` triggers `instantiate(...)`. See CajetaClass.h for the
// instantiation entry point and the cache.
//

#pragma once

#include <list>
#include <string>
#include "QualifiedName.h"

using namespace std;

namespace cajeta {

    // One declared template parameter — `T` or `<T extends Foo & Bar>`. Bounds
    // are resolved to QualifiedNamePtrs at capture time so we don't need to
    // hold ANTLR parse contexts beyond the per-module build.
    struct TypeParameter {
        string name;
        list<QualifiedNamePtr> bounds;   // empty = unbounded

        TypeParameter() = default;
        TypeParameter(string name) : name(std::move(name)) {}
    };

}
