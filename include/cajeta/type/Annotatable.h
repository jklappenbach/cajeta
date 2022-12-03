//
// Created by James Klappenbach on 2/19/22.
//

#pragma once

#include <set>
#include "QualifiedName.h"

using namespace std;

namespace cajeta {
    class Annotatable {
    protected:
        set<QualifiedName*> annotations;
        list<QualifiedName*> annotationList;
    public:
        Annotatable() { }

        Annotatable(set<QualifiedName*>& src) {
            annotations.insert(src.begin(), src.end());
        }

        void addAnnotation(QualifiedName* qName) {
            annotationList.push_back(qName);
            annotations.insert(qName);
        }

        set<QualifiedName*>& getAnnotations() {
            return annotations;
        }

        list<QualifiedName*>& getAnnotationList() { return annotationList; }
    };
}
