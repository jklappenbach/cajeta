//
// Created by James Klappenbach on 2/20/22.
//

#pragma once

#include <set>
#include "cajeta/field/Field.h"

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
    public:
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

        MethodPtr getParent() const;

        void setParent(MethodPtr parent);

        string toCanonical(bool labeled = false);

        static FormalParameterPtr fromContext(CajetaParser::FormalParameterContext* ctx, CajetaModulePtr module);

        string& getName();

        void setName(const string& name);

        CajetaTypePtr getType() const;

        void setType(CajetaTypePtr type);
    };
}