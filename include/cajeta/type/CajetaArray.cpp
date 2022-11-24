//
// Created by James Klappenbach on 10/2/22.
//

#include "cajeta/type/CajetaArray.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/method/ArrayDestructorMethod.h"

namespace cajeta {
    string CajetaArray::ARRAY_FIELD_NAME("@array");

    CajetaArray::CajetaArray(CajetaType* elementType, int dimension) {
        this->elementType = elementType;
        this->dimension = dimension;
        char typeName[1025];
        snprintf(typeName, 1024, "%s[%d]", elementType->getQName()->getTypeName().c_str(), dimension);
        qName = QualifiedName::getOrInsert(typeName, elementType->getQName()->getPackageName());
        canonical = qName->toCanonical();
        char fieldName[256];
        fields[ARRAY_FIELD_NAME] = new StructureField(ARRAY_FIELD_NAME, CajetaType::of("pointer"), 0);
        for (int i = 0; i < dimension; i++) {
            snprintf(fieldName, 255, "@dim%d", i);
            fields[fieldName] = new StructureField(fieldName, CajetaType::of("int32"), i + 1);
        }
    }

    void CajetaArray::ensureDefaultDestructor(CajetaModule* module) {
        string name = "~";
        name.append(qName->getTypeName());
        if (methods.find(name) == methods.end()) {
            addMethod(new ArrayDestructorMethod(this));
        }
    }
}