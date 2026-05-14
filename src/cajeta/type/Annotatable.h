//
// Created by James Klappenbach on 2/19/22.
//

#pragma once

#include <set>
#include <list>
#include <string>
#include <vector>
#include "QualifiedName.h"

using namespace std;

namespace cajeta {
    class Annotatable {
    protected:
        set<QualifiedNamePtr> annotations;
        list<QualifiedNamePtr> annotationList;
        // Lint rule IDs suppressed at this declaration via @SuppressLint(...).
        // See cajeta-docs/LintRules.md. Today only the visitor's @SuppressLint
        // handler populates this; future generic annotation-parameter capture
        // (AspectModel.md task A1) will fold this into a typed annotation-arg
        // table.
        vector<string> suppressedLints;
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

        void addSuppressedLint(const string& ruleId) {
            suppressedLints.push_back(ruleId);
        }

        bool isLintSuppressed(const string& ruleId) const {
            for (const auto& id : suppressedLints) {
                if (id == ruleId) return true;
            }
            return false;
        }

        const vector<string>& getSuppressedLints() const { return suppressedLints; }

        string toCanonical() {
            string result;
            for (QualifiedNamePtr qName : annotationList) {
                result.append(string("@") + qName->toCanonical()).append(" ");
            }
            return result;
        }
    };
}
