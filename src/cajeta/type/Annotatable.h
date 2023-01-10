//
// Created by James Klappenbach on 2/19/22.
//

#pragma once

#include <set>
#include <list>
#include "QualifiedName.h"

using namespace std;

namespace cajeta {
    class Annotatable {
    protected:
        set<QualifiedNamePtr> annotations;
        list<QualifiedNamePtr> annotationList;
    public:
        Annotatable() { }

        Annotatable(set<QualifiedNamePtr>& src) {
            annotations.insert(src.begin(), src.end());
        }

        void addAnnotation(QualifiedNamePtr qName) {
            annotationList.push_back(qName);
            annotations.insert(qName);
        }

        set<QualifiedNamePtr>& getAnnotations() {
            return annotations;
        }

        list<QualifiedNamePtr>& getAnnotationList() { return annotationList; }

        string toCanonical() {
            string result;
            for (QualifiedNamePtr qName : annotationList) {
                result.append(string("@") + qName->toCanonical()).append(" ");
            }
            return result;
        }
    };
}
