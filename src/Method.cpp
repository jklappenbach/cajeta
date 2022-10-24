//
// Created by James Klappenbach on 2/19/22.
//

#include "cajeta/type/Method.h"

using namespace std;

namespace cajeta {
    void CajetaType::addMethod(Method* method) {
        methods.insert(method);
    }
}