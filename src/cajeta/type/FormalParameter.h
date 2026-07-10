//
// Created by James Klappenbach on 2/20/22.
//

#pragma once

#include <set>
#include "cajeta/field/Field.h"

namespace cajeta {
    class Expression;
    typedef std::shared_ptr<Expression> ExpressionPtr;
}

using namespace std;

namespace cajeta {
    class CajetaModule;
    typedef shared_ptr<CajetaModule> CajetaModulePtr;

    class Method;
    typedef shared_ptr<Method> MethodPtr;

    class FormalParameter;
    typedef shared_ptr<FormalParameter> FormalParameterPtr;

    class FormalParameter : public Modifiable, public Annotatable {
    protected:
        MethodPtr parent;
        string name;
        CajetaTypePtr type;
        // True iff the parameter type is prefixed with `#` (e.g. `#String s`),
        // meaning the parameter takes ownership of its argument at the callsite.
        // See `MemoryModel.md` § Borrow / transfer rules.
        bool transferred = false;
        // `int32 x = 42` — default value expression. Evaluated at the call
        // site when the caller omits this argument. Stored as an AST node
        // so the evaluation is lazy (Python-like) and respects the
        // caller-side scope rather than a value frozen at declaration time.
        ExpressionPtr defaultValue;
        // element-ownership §3 (plan 4A): which enclosing-class type parameter
        // this formal's declared type came from (`put(#K k)` → index of K), or
        // -1. Recorded during the instantiation body walk — the monomorphized
        // type (`Elem`) no longer says it CAME FROM `K`, and the call-site
        // agreement check (4B) pairs the formal with isTypeArgumentOwning(i).
        // Mirrors StructureProperty's originTypeParamIndex (Unit-3B linkage).
        int originTypeParamIndex = -1;
        // The author's `#` as WRITTEN (`#K k`), never cleared. `transferred`
        // above is the EFFECTIVE mode after §4.2 dissolution — under a borrow
        // instantiation an authored `#K` dissolves (transferred=false) while
        // this stays true, so body call sites forwarding `#k` can dissolve
        // their `#` too instead of tripping BORROW_PARAM_ESCAPES; a plain-K
        // formal forwarded with `#` (authored=false) stays a genuine error.
        bool authoredTransferred = false;
    public:
        ExpressionPtr getDefaultValue() const { return defaultValue; }
        void setDefaultValue(ExpressionPtr e) { defaultValue = std::move(e); }

        FormalParameter(string name, CajetaTypePtr type, set<Modifier>& modifiers,
            set<QualifiedNamePtr>& annotations);

        FormalParameter(string name, CajetaTypePtr type) {
            this->name = name;
            this->type = type;
        }

        FormalParameter(const FormalParameter& src) {
            parent = src.parent;
            name = src.name;
            type = src.type;
            transferred = src.transferred;
            originTypeParamIndex = src.originTypeParamIndex;
            authoredTransferred = src.authoredTransferred;
        }

        bool isTransferred() const { return transferred; }
        void setTransferred(bool v) { transferred = v; }

        int getOriginTypeParamIndex() const { return originTypeParamIndex; }
        void setOriginTypeParamIndex(int idx) { originTypeParamIndex = idx; }

        // Authored `#` (as written); survives §4.2 dissolution. True with
        // transferred=false ⇔ this formal dissolved under a borrow mode.
        bool wasAuthoredTransferred() const { return authoredTransferred; }
        void setAuthoredTransferred(bool v) { authoredTransferred = v; }

        MethodPtr getParent() const;

        void setParent(MethodPtr parent);

        string toCanonical(bool labeled = false);

        static FormalParameterPtr fromContext(CajetaParser::FormalParameterContext* ctx, CajetaModulePtr module);

        // Build a FormalParameter from the `T... args` (varargs) form. The
        // resulting parameter's type is `T[]` (CajetaArray-wrapped); the
        // method-level varargs flag is tracked separately on the Method.
        // Callers (MethodCallExpression) pack trailing args into a fresh
        // T[] before passing.
        static FormalParameterPtr fromContext(CajetaParser::LastFormalParameterContext* ctx, CajetaModulePtr module);

        string& getName();

        void setName(const string& name);

        CajetaTypePtr getType() const;

        void setType(CajetaTypePtr type);
    };
}