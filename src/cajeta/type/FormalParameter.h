// FormalParameter - one declared parameter of a method.

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
        // `#String s` — the parameter takes ownership of its argument.
        bool transferred = false;
        // `int32 x = 42`, an AST node so it evaluates in the caller's scope.
        ExpressionPtr defaultValue;
        string declaredTypeParamName;   // empty = not T-var-typed
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

        // The method-level template parameter this formal was DECLARED with
        // (`toBytes<T>(T value)` -> "T"): the resolved CajetaTypePtr is not a
        // reliable record, since a shared placeholder can be refilled with a class.
        const string& getDeclaredTypeParamName() const {
            return declaredTypeParamName;
        }
        void setDeclaredTypeParamName(const string& n) {
            declaredTypeParamName = n;
        }

        MethodPtr getParent() const;

        void setParent(MethodPtr parent);

        // Signature text: annotations, then modifiers, then the canonical type.
        // `labeled` REPLACES that prefix with `name: `, so a labeled canonical
        // carries no annotation or modifier text.
        string toCanonical(bool labeled = false);

        // One declared parameter from its parse tree: type, modifiers, annotation
        // instances, the `#` transfer flag, and the default value kept as an AST node.
        // Null when the type does not resolve, after CAJETA_ERROR_UNRESOLVED_TYPE.
        static FormalParameterPtr fromContext(CajetaParser::FormalParameterContext* ctx, CajetaModulePtr module);

        // The `T... args` varargs form, whose parameter type is `T[]`; the
        // method-level varargs flag is tracked separately on the Method.
        static FormalParameterPtr fromContext(CajetaParser::LastFormalParameterContext* ctx, CajetaModulePtr module);

        string& getName();

        void setName(const string& name);

        CajetaTypePtr getType() const;

        void setType(CajetaTypePtr type);
    };
}