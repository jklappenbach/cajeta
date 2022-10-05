//
// Created by James Klappenbach on 10/1/22.
//

#pragma once

#include <set>
#include "Type.h"
#include "Class.h"
#include "Enum.h"

using namespace std;

namespace cajeta {
    class CompilationUnit {
    private:
        string packageName;
        set<QualifiedName*> imports;
        map<QualifiedName*, Type*> types;
    public:
        const string& getPackageName() const;

        void setPackageName(const string& packageName);

        set<QualifiedName*>& getImports();

        void setImports(const set<QualifiedName*>& imports);

        map<QualifiedName*, Type*>& getTypes();

        void setTypes(const map<QualifiedName*, Type*>& types);
    };
}
