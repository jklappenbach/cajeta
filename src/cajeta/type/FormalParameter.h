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
        // See getDeclaredTypeParamName below. Empty = not T-var-typed.
        string declaredTypeParamName;
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
        }

        bool isTransferred() const { return transferred; }
        void setTransferred(bool v) { transferred = v; }

        // Set at DECLARATION time when this formal's type is a method-level
        // template parameter (`toBytes<T>(T value)` -> "T"): the visitor
        // compares the formal's resolved type against the placeholder it
        // just pushed for each T-var. The resolved CajetaTypePtr is NOT a
        // reliable record of this fact — the shared placeholder machinery
        // can later fill/replace that object with an unrelated concrete
        // class (observed: the buildtool's first Json.toBytes<Finding>
        // instantiation left the TEMPLATE's formal claiming 'Finding', so
        // every codec body synthesizer declined and the placeholder
        // throw-body shipped). This name is immutable once captured.
        const string& getDeclaredTypeParamName() const {
            return declaredTypeParamName;
        }
        void setDeclaredTypeParamName(const string& n) {
            declaredTypeParamName = n;
        }

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