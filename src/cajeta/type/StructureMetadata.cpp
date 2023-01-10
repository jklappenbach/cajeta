//
// Created by James Klappenbach on 11/20/22.
//

#include "StructureMetadata.h"

namespace cajeta {

    /**
     * 1. Parameter Name (string, array)
     * 2. Parameter Type (string, array)
     * 3. Modifier Count (int8)
     * 4. Modifiers (int8, array)
     * 5. Annotation Count (int8)
     * 6. Annotation Types (array of strings)
     *
     * @param module
     */
    llvm::Type* StructureMetadata::createPropertyType(CajetaClassPtr structure, StructurePropertyPtr property) {
        vector<llvm::Type*> members;
        members.push_back(llvm::ArrayType::get(llvmInt8Type, property->getName().size() + 1));
        members.push_back(llvm::ArrayType::get(llvmInt8Type, property->getType()->toCanonical().size() + 1));
        members.push_back(llvm::IntegerType::getInt8Ty(*module->getLlvmContext()));
        members.push_back(llvm::ArrayType::get(llvmInt8Type, property->getModifiers().size()));
        members.push_back(llvm::IntegerType::getInt8Ty(*module->getLlvmContext()));
        vector<llvm::Type*> annotationStringTypes;
        for (auto& qName: property->getAnnotationList()) {
            annotationStringTypes.push_back(llvm::ArrayType::get(llvmInt8Type, qName->toCanonical().size() + 1));
        }
        members.push_back(llvm::StructType::get(*module->getLlvmContext(), annotationStringTypes));

        return llvm::StructType::create(*module->getLlvmContext(),
            llvm::ArrayRef(members),
            structure->toCanonical() + "::" + property->getName() + string(".#Metadata"));
    }

    llvm::Constant* StructureMetadata::createPropertyConstant(StructurePropertyPtr property, llvm::StructType* llvmPropertyType) {
        vector<llvm::Constant*> args;
        args.push_back(llvm::ConstantDataArray::getString(*module->getLlvmContext(),
            property->getName(),
            true));
        args.push_back(llvm::ConstantDataArray::getString(*module->getLlvmContext(),
            property->getType()->toCanonical(),
            true));
        args.push_back(llvm::ConstantInt::get(llvmInt8Type,
            llvm::APInt(8, property->getModifiers().size(), false)));
        vector<llvm::Constant*> modifiers;
        for (auto& modifier: property->getModifiers()) {
            modifiers.push_back(llvm::ConstantInt::get(llvmInt8Type,
                llvm::APInt(8, modifier, false)));
        }
        args.push_back(llvm::ConstantArray::get(llvm::ArrayType::get(llvmInt8Type, property->getModifiers().size()),
            llvm::ArrayRef<llvm::Constant*>(modifiers)));

        args.push_back(llvm::ConstantInt::get(llvmInt8Type,
            llvm::APInt(8, property->getAnnotations().size(), false)));
        vector<llvm::Constant*> annotations;
        for (auto& annotation: property->getAnnotations()) {
            annotations.push_back(llvm::ConstantDataArray::getString(*module->getLlvmContext(),
                annotation->toCanonical(),
                true));
        }

        args.push_back(llvm::ConstantStruct::get((llvm::StructType*) llvmPropertyType->getTypeAtIndex(5),
            llvm::ArrayRef<llvm::Constant*>(annotations)));

        return llvm::ConstantStruct::get(llvmPropertyType, llvm::ArrayRef<llvm::Constant*>(args));
    }

    /**
     * 1. Parameter Name (string, array)
     * 2. Parameter Type (string, array)
     * 3. Modifier Count (int8)
     * 4. Modifiers (int8, array)
     * 5. Annotation Count (int8)
     * 6. Structure annotations (array of strings)
     *
     * @param parameter The parameter to generate a type
     * @return llvm::StructType of the parameter metadata
     */
    llvm::StructType* StructureMetadata::createParameterType(FormalParameterPtr parameter) {
        vector<llvm::Type*> members;
        members.push_back(llvm::ArrayType::get(llvmInt8Type, parameter->getName().size() + 1));
        members.push_back(llvm::ArrayType::get(llvmInt8Type, parameter->getType()->toCanonical().size() + 1));
        members.push_back(llvmInt8Type);
        members.push_back(llvm::ArrayType::get(llvmInt8Type, parameter->getModifiers().size()));
        members.push_back(llvmInt8Type);
        vector<llvm::Type*> annotationStringTypes;
        for (auto& qName: parameter->getAnnotationList()) {
            annotationStringTypes.push_back(llvm::ArrayType::get(llvmInt8Type, qName->toCanonical().size() + 1));
        }
        members.push_back(llvm::StructType::get(*module->getLlvmContext(), annotationStringTypes));
        return llvm::StructType::create(*module->getLlvmContext(),
            llvm::ArrayRef(members),
            parameter->getParent()->toCanonical() + parameter->getName() +
                string(".#ParameterMetadata"));
    }

    /**
     * 1. Parameter Name (string, array)
     * 2. Parameter Type (string, array)
     * 3. Modifier Count (int8)
     * 4. Modifiers (int8, array)
     * 5. Annotation Count (int8)
     * 6. Structure annotations (array of strings)
     *
     * @param parameter The parameter to generate a type
     * @return llvm::StructType of the parameter metadata
     */
    llvm::Constant* StructureMetadata::createParameterConstant(FormalParameterPtr parameter, llvm::StructType* parameterType) {
        vector<llvm::Constant*> args;
        args.push_back(llvm::ConstantDataArray::getString(*module->getLlvmContext(), parameter->getName(), true));
        args.push_back(
            llvm::ConstantDataArray::getString(*module->getLlvmContext(), parameter->getType()->toCanonical(),
                true));
        args.push_back(llvm::ConstantInt::get(llvmInt8Type, llvm::APInt(8, parameter->getModifiers().size(), false)));
        vector<llvm::Constant*> modifiers;
        for (auto& modifier: parameter->getModifiers()) {
            modifiers.push_back(llvm::ConstantInt::get(llvmInt8Type, llvm::APInt(8, modifier, false)));
        }
        args.push_back(llvm::ConstantArray::get((llvm::ArrayType*) parameterType->getTypeAtIndex(3),
            llvm::ArrayRef<llvm::Constant*>(modifiers)));

        args.push_back(llvm::ConstantInt::get(llvm::IntegerType::getInt8Ty(*module->getLlvmContext()),
            llvm::APInt(8, parameter->getAnnotations().size(), false)));
        vector<llvm::Constant*> annotations;
        for (auto& annotation: parameter->getAnnotations()) {
            annotations.push_back(llvm::ConstantDataArray::getString(*module->getLlvmContext(),
                annotation->toCanonical(),
                true));
        }

        args.push_back(llvm::ConstantStruct::get((llvm::StructType*) parameterType->getTypeAtIndex(6),
            llvm::ArrayRef<llvm::Constant*>(annotations)));

        return llvm::ConstantStruct::get(parameterType, llvm::ArrayRef<llvm::Constant*>(args));
    }

    /**
     * 1. Method name
     * 2. Return type
     * 2. Number of parameters
     * 3. Structure of parameters
     *
     * @param method
     */
    llvm::StructType* StructureMetadata::createMethodType(MethodPtr method) {
        vector<llvm::Type*> members;
        members.push_back(llvm::ArrayType::get(llvmInt8Type, method->toGeneric().size()));
        members.push_back(llvm::ArrayType::get(llvmInt8Type, method->getReturnType()->toCanonical().size()));
        members.push_back(llvmInt16Type);
        vector<llvm::Type*> parameterTypes;
        for (auto& parameter: method->getParameterList()) {
            parameterTypes.push_back(createParameterType(parameter));
        }
        members.push_back(
            llvm::StructType::get(*module->getLlvmContext(), llvm::ArrayRef<llvm::Type*>(parameterTypes)));
        return llvm::StructType::create(*module->getLlvmContext(),
            llvm::ArrayRef(members),
            method->toCanonical() + string("#MethodMetadata"));
    }

    llvm::Constant* StructureMetadata::createMethodConstant(MethodPtr method, llvm::StructType* llvmMethodType) {
        vector<llvm::Constant*> args;
        args.push_back(llvm::ConstantDataArray::getString(*module->getLlvmContext(),
            method->toCanonical(),
            true));
        args.push_back(llvm::ConstantDataArray::getString(*module->getLlvmContext(),
            method->getReturnType()->toCanonical(),
            true));
        args.push_back(
            llvm::ConstantInt::get(llvmInt16Type, llvm::APInt(16, method->getParameterList().size(), false)));
        vector<llvm::Constant*> parameterConstants;
        llvm::StructType* parameterTypes = (llvm::StructType*) llvmMethodType->getTypeAtIndex(3);
        int i = 0;
        for (auto& parameter: method->getParameterList()) {
            parameterConstants.push_back(
                createParameterConstant(parameter, (llvm::StructType*) parameterTypes->getTypeAtIndex(i++)));
        }
        args.push_back(llvm::ConstantStruct::get(parameterTypes, llvm::ArrayRef<llvm::Constant*>(parameterConstants)));

        return llvm::ConstantStruct::get(llvmMethodType, llvm::ArrayRef<llvm::Constant*>(args));
    }

    /**
     * 1. Type name
     * 2. Size of property list
     * 3. Structure of properties
     * 4. Size of class method list
     * 5. Structure of methods
     * 6. Size of superclass list
     * 7. Structure of parent types
     *
     * @param module
     */
    void StructureMetadata::createRttiType(CajetaClassPtr structure) {
        vector<llvm::Type*> members;

        // 0. Version ID
        members.push_back(llvmInt16Type);

        // 1. Type name
        members.push_back(llvm::ArrayType::get(llvm::Type::getInt8Ty(*module->getLlvmContext()),
            structure->toCanonical().size() + 1));

        // 2. Size of property list
        members.push_back(llvmInt16Type);

        // 3. List of properties
        vector<llvm::Type*> propertyTypes;
        for (auto& property: structure->getPropertyList()) {
            propertyTypes.push_back(createPropertyType(structure, property));
        }

//        // 4. Create the struct
//        this->llvmPropertiesType = llvm::StructType::create(*pModule->getLlvmContext(), propertyTypes);
//
//        // 5. Add that as the first structure of properties
//        members.push_back(llvmPropertiesType);
//
//        // 6. Size of class method list
//        members.push_back(llvmInt16Type);
//
//        // 7. List of methods
//        vector<llvm::Type*> methodTypes;
//        for (auto& method: structure->getMethodList()) {
//            methodTypes.push_back(createMethodType(method));
//        }
//
//        // 8. Add methodTypes to rtti of method types
//        members.push_back(llvm::StructType::get(*pModule->getLlvmContext(), methodTypes));
//        llvmRttiType = llvm::StructType::create(*pModule->getLlvmContext(), llvm::ArrayRef(members),
//            structure->toCanonical() + string("#RttiType"));
    }

    llvm::Constant* StructureMetadata::createRttiConstant(vector<llvm::Constant*>& args, CajetaClassPtr structure) {
        // 0. Version ID
        args.push_back(llvm::ConstantInt::get(llvmInt16Type, llvm::APInt(16, 0, false)));
        // 1. Type name
        args.push_back(llvm::ConstantDataArray::getString(*module->getLlvmContext(), structure->toCanonical(), true));

        // 2. Size of property list
        args.push_back(llvm::ConstantInt::get(llvmInt16Type, llvm::APInt(16, structure->getProperties().size(), false)));

        // 3. Structure of properties
        vector<llvm::Constant*> propertyConstants;
        int i = 0;
        for (auto& property: structure->getPropertyList()) {
            propertyConstants.push_back(
                createPropertyConstant(property, (llvm::StructType*) llvmPropertiesType->getTypeAtIndex(i++)));
        }

        args.push_back(
            llvm::ConstantArray::get(
                llvm::ArrayType::get(llvmPropertiesType, propertyConstants.size()),
                llvm::ArrayRef<llvm::Constant*>(propertyConstants)
            )
        );

        // 4. Create the struct

        // 5. Add that as the first structure of properties

        // 6. Size of class method list

        // 7. List of methods

        // 8. Add methodTypes to rtti of method types


//
//        // 4. Size of class method list
//        args.push_back(llvm::ConstantInt::get(llvmInt16Type, llvm::APInt(16, structure->getMethods().size(), false)));

//        vector<llvm::Constant*> methodConstants;
//        for (auto &method : structure->getMethodList()) {
//            methodConstants.push_back(createMethodConstant(method));
//        }
//        // 5. Structure of methods
//        args.push_back(
//            llvm::ConstantArray::get(
//                    llvm::ArrayType::get(llvmMethodType, methodConstants.size()),
//                    llvm::ArrayRef<llvm::Constant*>(methodConstants)
//            )
//        );
//
//        args.push_back(llvm::ConstantInt::get(llvmInt16Type, llvm::APInt(16, structure->getSuperClasses().size(), false)));
//        vector<llvm::Constant*> superConstants;
//        for (auto &super : structure->getSuperClasses()) {
//            superConstants.push_back(llvm::ConstantDataArray::getString(*pModule->getLlvmContext(),
//                                                                        super->toCanonical(),
//                                                                        true));
//        }
//        args.push_back(
//                llvm::ConstantArray::get(
//                        llvm::ArrayType::get(llvm::PointerType::getInt64PtrTy(*pModule->getLlvmContext()), methodConstants.size()),
//                        llvm::ArrayRef<llvm::Constant*>(methodConstants)
//                )
//        );

        return llvm::ConstantStruct::get(llvmRttiType, llvm::ArrayRef<llvm::Constant*>(args));
    }

    void StructureMetadata::populate(CajetaClassPtr structure) {
        string globalName = structure->toCanonical() + string("#VTable");
        if (module->getModuleVariables().find(globalName) == module->getModuleVariables().end()) {
            llvm::GlobalVariable* virtualTableGlobal = module->getLlvmModule()->getGlobalVariable(globalName);
            if (!virtualTableGlobal) {
                createVirtualTableType(structure);
                virtualTableGlobal = (llvm::GlobalVariable*) module->getLlvmModule()->
                    getOrInsertGlobal(globalName, structure->getVirtualTableType());
                llvm::Constant* virtualTableConstant = createVirtualTableConstant(structure);
                virtualTableGlobal->setInitializer(virtualTableConstant);
                structure->setVirtualTableGlobal(virtualTableGlobal);
            }
            module->getModuleVariables()[globalName] = module;
        }

//        createRttiType(structure);
//
//        vector<llvm::Constant*> args;
//        llvm::GlobalVariable* rttiGlobal = (llvm::GlobalVariable*) pModule->getLlvmModule()->getOrInsertGlobal(
//            llvm::StringRef(structure->toCanonical() + string("#RttiGlobal")), llvmRttiType);
//        rttiGlobal->setInitializer(createRttiConstant(args, structure));
//        structure->setRttiGlobal(rttiGlobal);
    }

    bool compareMethod(MethodPtr first, MethodPtr second) {
        return first->getVirtualTableIndex() < second->getVirtualTableIndex();
    }


    /**
     * 0. Version
     * 1. TODO: Convert function pointers to an array
     * 2. TODO: Make sure structure methods are set to the correct index in the table
     * @param structure
     * @return
     */
    llvm::Type* StructureMetadata::createVirtualTableType(CajetaClassPtr structure) {
        vector<llvm::Type*> members;
        structure->createInheritanceMethodMap();
        list<MethodPtr>& sortedMethods = structure->getMethodList();
        for (auto& entry : structure->getUnlabeledMethodMap()) {
            for (auto& preciseEntry : (entry.second)) {
                sortedMethods.push_back(preciseEntry.second);
            }
        }

        sortedMethods.sort(compareMethod);

        // 0. Version
        members.push_back(llvmInt16Type);

        // 1. Number of functions
        members.push_back(llvmInt16Type);

        // 2. List of function types
        for (auto& method : sortedMethods) {
            members.push_back(method->getLlvmFunctionType());
        }
        llvm::StructType* result = llvm::StructType::create(*module->getLlvmContext(), llvm::ArrayRef(members), structure->toCanonical() + string("#VTable"));
        structure->setVirtualTableType(result);
        return result;
    }


    llvm::Constant* StructureMetadata::createVirtualTableConstant(CajetaClassPtr structure) {
        vector<llvm::Constant*> args;

        createVirtualTableType(structure);

        // 0. Version
        args.push_back(llvm::ConstantInt::get(llvmInt16Type, llvm::APInt(16, 0, false)));

        // 1. Number of functions
        args.push_back(llvm::ConstantInt::get(llvmInt16Type,
            llvm::APInt(16, structure->getVirtualMethodList().size(), false)));

        // 2. List of functions
        for (auto& method : structure->getVirtualMethodList()) {
            args.push_back(method->getLlvmFunction());
        }
        return llvm::ConstantStruct::get(structure->getVirtualTableType(), llvm::ArrayRef<llvm::Constant*>(args));
    }
} // code