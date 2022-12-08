//
// Created by James Klappenbach on 10/2/22.
//

#include "cajeta/type/CajetaArray.h"
#include "cajeta/compile/CajetaModule.h"

namespace cajeta {
    string CajetaArray::ARRAY_FIELD_NAME("#array");

    CajetaArray::CajetaArray(CajetaModule* module, CajetaType* elementType, int dimension) : CajetaStructure(module) {
        this->elementType = elementType;
        this->dimension = dimension;
        char typeName[1025];
        snprintf(typeName, 1024, "%s[%d]", elementType->getQName()->getTypeName().c_str(), dimension);
        qName = QualifiedName::getOrInsert(typeName, elementType->getQName()->getPackageName());
        canonical = qName->toCanonical();
        char fieldName[256];
        CajetaType* arrayPropertyType = new CajetaType(elementType->getQName()->toArrayType(),
            elementType->getLlvmType()->getPointerTo());
        properties[ARRAY_FIELD_NAME] = new ClassProperty(ARRAY_FIELD_NAME, arrayPropertyType, 0);
        for (int i = 0; i < dimension; i++) {
            snprintf(fieldName, 255, "#dim%d", i);
            properties[fieldName] = new ClassProperty(fieldName, CajetaType::of("int64"), i + 1);
        }
    }
}