//
// Created by James Klappenbach on 3/15/23.
//

#pragma once

#include "Scope.h"

namespace cajeta {
    class ScopeStack {
    private:
        list<ScopePtr> stack;
    public:
        void add(ScopePtr scope) {
            if (!stack.empty()) {
                scope->setParent(stack.back());
            }
            stack.push_back(scope);
        }

        ScopePtr peek() {
            return stack.back();
        }

        bool isEmpty() {
            return stack.empty();
        }

        ScopePtr pop() {
            ScopePtr scope = stack.back();
            stack.pop_back();
            return scope;
        }

        // Save & clear — for cross-method codegen barriers (recursive
        // method-template instantiation mid-codegen of a caller, where
        // the inner method's resolve-types must NOT fall through the
        // parent chain to the caller's locals). Returns the entire
        // stack contents; restore() puts them back. Between save and
        // restore the stack is empty, so subsequent `add()` calls
        // create root-scoped frames with no parent.
        list<ScopePtr> save() {
            list<ScopePtr> out;
            out.swap(stack);
            return out;
        }
        void restore(list<ScopePtr> saved) {
            stack.swap(saved);
        }
    };
}
