//
// Created by James Klappenbach on 2/19/22.
//

#include "cajeta/Method.h"

using namespace std;

namespace cajeta {
    void Type::addMethod(Method* method) {
        methods.insert(method);
    }
}