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

        // Non-type (value) parameter — `<..., uint32 N>`. When true the
        // parameter binds to a compile-time integer constant (a
        // CajetaConstantType argument), not a type; `nonTypePrimitive` records
        // the declared primitive (e.g. "uint32"). The built-in
        // `CooperativeMatrix<T, uint32 Rows, uint32 Cols, uint32 Use>` and any
        // future user template (`Matrix<T, uint32 R, uint32 C>`) use this.
        // Non-type params carry no `bounds`, so the TPL-6 bound check skips
        // them; the instantiator instead checks the arg is a constant.
        bool isNonType = false;
        string nonTypePrimitive;         // declared primitive name when isNonType

        // element-ownership §4.1.5 — declaration-`#`: `class Cache<#K, V>` marks
        // K owning-required (non-dissolvable, contagious through inheritance,
        // §8.6). Distinct from a use-site `#K` marker, which permits owning and
        // dissolves to a borrow. Set by the class-declaration visitor from the
        // `REFERENCE?` prefix on the `typeParameter` grammar rule (§8.4.2).
        bool owningRequired = false;

        // Default type argument — `<T = float32>`. The text of the default
        // `typeType` (e.g. "float32"), captured at parse time (no module is
        // available at the prescan capture site, so it can't be resolved to a
        // CajetaType here) and resolved against `CajetaType::canonicalMap` when
        // an instantiation omits the trailing argument. Empty = no default.
        // Defaults must be TRAILING (a non-defaulted param can't follow a
        // defaulted one); the instantiator fills omitted trailing args and the
        // arity check rejects a gap that no default covers. Type params only
        // (v1) — non-type defaults (`uint32 N = 4`) are not parsed.
        string defaultType;

        TypeParameter() = default;
        TypeParameter(string name) : name(std::move(name)) {}
    };

}
