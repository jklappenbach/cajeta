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
    };
}
