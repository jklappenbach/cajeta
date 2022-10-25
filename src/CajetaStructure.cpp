//
// Created by James Klappenbach on 10/24/22.
//

#include "cajeta/type/CajetaStructure.h"
#include "cajeta/type/Field.h"
#include "cajeta/type/Method.h"

namespace cajeta {
    void CajetaStructure::addField(Field* field) {
        this->fields[field->getName()] = field;
    }
    void CajetaStructure::addFields(list<Field*> fields) {
        for (Field* field : fields) {
            this->fields[field->getName()] = field;
        }
    }
    void CajetaStructure::addMethod(Method* method) {
        this->methods[method->getName()] = method;
    }
    void CajetaStructure::addMethods(list<Method*> methods) {
        for (Method* method : methods) {
            this->methods[method->getName()] = method;
        }
    }
} // cajeta