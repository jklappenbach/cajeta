//
// Created by James Klappenbach on 10/2/22.
//

#include "CajetaArray.h"
#include "../compile/CajetaModule.h"

namespace cajeta {
    string CajetaArray::ARRAY_FIELD_NAME("#array");

    CajetaArray::CajetaArray(CajetaModulePtr module, CajetaTypePtr elementType, int dimension) : CajetaStructure(module) {
        this->elementType = elementType;
        this->dimension = dimension;
        string typeName = elementType->toCanonical();
        for (int i = 0; i < dimension; i++) {
            typeName.append(string("[]"));
        }
        qName = QualifiedName::getOrCreate(typeName);
        canonical = qName->toCanonical();

        // First, create the common array type
        vector<llvm::Type*> structProps;
        char fieldName[256];
        CajetaTypePtr arrayPropertyType = CajetaType::create(elementType->getQName()->toArrayType(),
            elementType->getLlvmType()->getPointerTo(), REFERENCE_FLAG);

        properties[ARRAY_FIELD_NAME] = make_shared<StructureProperty>(ARRAY_FIELD_NAME, arrayPropertyType, 0);
        structProps.push_back(elementType->getLlvmType()->getPointerTo());
        for (int i = 0; i < dimension; i++) {
            snprintf(fieldName, 255, "#dim%d", i);
            properties[string(fieldName)] = make_shared<StructureProperty>(fieldName, CajetaType::of("int64"), i + 1);
            structProps.push_back(CajetaType::of("int64")->getLlvmType());
        }
        llvmType = CajetaType::getOrCreateLlvmType(module->getLlvmContext(), canonical, structProps);

        // Create the reference type
        vector<llvm::Type*> referenceProps;
        referenceProps.push_back(llvm::Type::getInt1Ty(*module->getLlvmContext()));  // Reference owner?
        referenceProps.push_back(llvmType);
        llvmReferenceType = CajetaType::getOrCreateLlvmType(module->getLlvmContext(), string("#") + canonical, referenceProps);
    }

    CajetaArray::CajetaArray(CajetaModulePtr module, CajetaTypePtr elementType, vector<long> dimensions) : CajetaStructure(module) {
        this->elementType = elementType;
        this->dimension = dimensions.size();
        string typeName = elementType->toCanonical();
        long totalElements = 0;
        for (long dim : dimensions) {
            typeName.append(string("[")).append(std::to_string(dim)).append(string("]"));
            totalElements += dim;
        }
        qName = QualifiedName::getOrCreate(typeName);
        canonical = qName->toCanonical();

        // First, create the common array type
        vector<llvm::Type*> structProps;
        char fieldName[256];
        llvm::Type* llvmArrayType = llvm::ArrayType::get(elementType->getLlvmType(), totalElements);
        CajetaTypePtr arrayType = make_shared<CajetaType>(string("#") + typeName, llvmArrayType, USER_DEFINED_FLAG);
        properties[ARRAY_FIELD_NAME] = make_shared<StructureProperty>(ARRAY_FIELD_NAME, arrayType, 0);
        structProps.push_back(llvmArrayType);
        for (int i = 0; i < dimension; i++) {
            snprintf(fieldName, 255, "#dim%d", i);
            properties[string(fieldName)] = make_shared<StructureProperty>(fieldName, CajetaType::of("int64"), i + 1);
            structProps.push_back(CajetaType::of("int64")->getLlvmType());
        }
        llvmType = CajetaType::getOrCreateLlvmType(module->getLlvmContext(), canonical, structProps);
    }
}