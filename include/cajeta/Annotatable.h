//
// Created by James Klappenbach on 2/19/22.
//

#pragma once
#include <set>
#include "QualifiedName.h"

using namespace std;

namespace cajeta {
    class Annotatable {
    private:
        set<QualifiedName*> annotations;
    public:
        Annotatable() { }
        Annotatable(set<QualifiedName*>& src) {
            annotations.merge(src);
        }
        void addAnnotation(QualifiedName* qName) {
            annotations.insert(qName);
        }
    };
}
