//
// Created by James Klappenbach on 11/20/22.
//

#include "StructureMetadata.h"

namespace cajeta {

    /**
 * 1. Parameter Name (string, array)
 * 2. Parameter Type (string, array)
 * 3. Modifier Count (int16)
 * 4. Modifiers (int16, array)
 * 5. Annotation Count (int16)
 * 6. Annotation Types (array of strings)
 *
 * @param module
 */
    void StructureMetadata::createFieldType(CajetaModule* module) {
        vector<llvm::Type*> members;
        members.push_back(llvm::PointerType::get(*module->getLlvmContext(), 0));
        members.push_back(llvm::PointerType::get(*module->getLlvmContext(), 0));
        members.push_back(llvm::IntegerType::getInt16Ty(*module->getLlvmContext()));
        members.push_back(llvm::PointerType::get(*module->getLlvmContext(), 0));
        members.push_back(llvm::IntegerType::getInt16Ty(*module->getLlvmContext()));
        members.push_back(llvm::PointerType::get(*module->getLlvmContext(), 0));
        llvmParameterType = llvm::StructType::create(*module->getLlvmContext(),
                                                     llvm::ArrayRef(members),
                                                     string("@FieldMetadata"));
    }

    llvm::Constant* StructureMetadata::createFieldConstant(StructureField* field, CajetaModule* module) {
        vector<llvm::Constant*> args;
        args.push_back(llvm::ConstantDataArray::getString(*module->getLlvmContext(),
                                                          field->getName(),
                                                          true));
        args.push_back(llvm::ConstantDataArray::getString(*module->getLlvmContext(),
                                                          field->getType()->toCanonical(),
                                                          true));
        args.push_back(llvm::ConstantInt::get(llvm::IntegerType::getInt16Ty(*module->getLlvmContext()),
                                              llvm::APInt(16, field->getModifiers().size(), false)));
        vector<llvm::Constant*> modifiers;
        for (auto &modifier : field->getModifiers()) {
            modifiers.push_back(llvm::ConstantInt::get(llvm::IntegerType::getInt16Ty(*module->getLlvmContext()),
                                                       llvm::APInt(16, modifier, false)));
        }
        args.push_back(llvm::ConstantArray::get(llvm::ArrayType::get(llvm::IntegerType::getInt16Ty(*module->getLlvmContext()),
                                                                     field->getModifiers().size()), llvm::ArrayRef<llvm::Constant*>(modifiers)));
        return llvm::ConstantStruct::get(llvmParameterType, llvm::ArrayRef<llvm::Constant*>(args));
    }

    /**
     * 1. Parameter Name (string, array)
     * 2. Parameter Type (string, array)
     * 3. Modifier Count (int16)
     * 4. Modifiers (int16, array)
     * 5. Annotation Count (int16)
     * 6. Annotation Types (array of strings)
     *
     * @param module
     */
    void StructureMetadata::createParameterType(CajetaModule* module) {
        vector<llvm::Type*> members;
        members.push_back(llvm::PointerType::get(*module->getLlvmContext(), 0));
        members.push_back(llvm::PointerType::get(*module->getLlvmContext(), 0));
        members.push_back(llvm::IntegerType::getInt16Ty(*module->getLlvmContext()));
        members.push_back(llvm::PointerType::get(*module->getLlvmContext(), 0));
        members.push_back(llvm::IntegerType::getInt16Ty(*module->getLlvmContext()));
        members.push_back(llvm::PointerType::get(*module->getLlvmContext(), 0));
        llvmParameterType = llvm::StructType::create(*module->getLlvmContext(),
                                                     llvm::ArrayRef(members),
                                                     string("@ParameterMetadata"));
    }

    llvm::Constant* StructureMetadata::createParameterConstant(FormalParameter* parameter, CajetaModule* module) {
        vector<llvm::Constant*> args;
        args.push_back(llvm::ConstantDataArray::getString(*module->getLlvmContext(), parameter->getName(), true));
        args.push_back(llvm::ConstantDataArray::getString(*module->getLlvmContext(), parameter->getType()->toCanonical(), true));
        args.push_back(llvm::ConstantInt::get(llvm::IntegerType::getInt16Ty(*module->getLlvmContext()),
                                              llvm::APInt(16, parameter->getModifiers().size(), false)));
        vector<llvm::Constant*> modifiers;
        for (auto &modifier : parameter->getModifiers()) {
            modifiers.push_back(llvm::ConstantInt::get(llvm::IntegerType::getInt16Ty(*module->getLlvmContext()),
                                                       llvm::APInt(16, modifier, false)));
        }
        args.push_back(llvm::ConstantInt::get(llvm::IntegerType::getInt16Ty(*module->getLlvmContext()),
                                              llvm::APInt(16, parameter->getModifiers().size(), false)));
        return llvm::ConstantStruct::get(llvmParameterType, llvm::ArrayRef<llvm::Constant*>(args));
    }

    /**
     * 1. Method name
     * 2. Return type
     * 2. Number of parameters
     * 3. Array of parameters
     *
     * @param module
     */
    void StructureMetadata::createMethodType(CajetaModule* module) {
        vector<llvm::Type*> members;
        members.push_back(llvm::PointerType::get(*module->getLlvmContext(), 0));
        members.push_back(llvm::PointerType::get(*module->getLlvmContext(), 0));
        members.push_back(llvm::IntegerType::getInt16Ty(*module->getLlvmContext()));
        members.push_back(llvm::PointerType::get(*module->getLlvmContext(), 0));
        llvmFieldType = llvm::StructType::create(*module->getLlvmContext(),
                                                 llvm::ArrayRef(members),
                                                 string("@MethodMetadata"));
    }

    /**
     * TODO: Need to create a custom structure for the vtable for a given class, needs to use the FunctionType for each method
     * in the struct.
     * @param module
     */
    void StructureMetadata::createVirtualTableType(CajetaStructure* structure, vector<Method*> methods, CajetaModule* module) {
        vector<llvm::Type*> members;
        for (auto &method : methods) {
            members.push_back(method->getLlvmFunctionType());
        }
        structure->setVirtualTableType(llvm::StructType::create(*module->getLlvmContext(), llvm::ArrayRef(members), string("@") +
                                                                                                                    structure->toCanonical() + string("VirtualTable")));
    }

    llvm::Constant* StructureMetadata::createVirtualTableConstant(llvm::StructType* llvmVirtualTableType, vector<Method*>& methods) {
        vector<llvm::Constant*> args;
        for (auto &method : methods) {
            args.push_back(method->getLlvmFunction());
        }
        return llvm::ConstantStruct::get(llvmVirtualTableType, llvm::ArrayRef<llvm::Constant*>(args));
    }

    llvm::Constant* StructureMetadata::createMethodConstant(Method* method, CajetaModule* module) {
        vector<llvm::Constant*> args;
        args.push_back(llvm::ConstantDataArray::getString(*module->getLlvmContext(),
                                                          method->toCanonical(),
                                                          true));
        args.push_back(llvm::ConstantDataArray::getString(*module->getLlvmContext(),
                                                          method->getReturnType()->toCanonical(),
                                                          true));
        args.push_back(llvm::ConstantInt::get(llvmInt16Type, llvm::APInt(16, method->getParameters().size(), false)));
        vector<llvm::Constant*> parameterConstants;
        for (auto &parameter : method->getParameters()) {
            parameterConstants.push_back(createParameterConstant(parameter, module));
        }
        return llvm::ConstantStruct::get(llvmMethodType, llvm::ArrayRef<llvm::Constant*>(args));
    }

    /**
     * 1. Array of Function pointers (vtable)
     * 2. Array of FunctionType pointers
     * 3. Type name
     * 4. Size of fields
     * 5. Array of fields
     * 6. Size of class method set
     * 7. Array of methods
     * 8. Size of parent structures
     * 9. Pointers to parent structures (array)
     *
     * @param module
     */
    void StructureMetadata::createRttiType(CajetaModule* module) {
        vector<llvm::Type*> members;

        members.push_back(llvm::PointerType::get(*module->getLlvmContext(), 0));
        members.push_back(llvm::PointerType::get(*module->getLlvmContext(), 0));

        members.push_back(llvm::PointerType::get(*module->getLlvmContext(), 0));
        members.push_back(llvmInt16Type);
        members.push_back(llvmFieldType->getPointerTo());
        members.push_back(llvmInt16Type);
        members.push_back(llvmMethodType->getPointerTo());
        members.push_back(llvmInt16Type);
        members.push_back(llvm::PointerType::get(*module->getLlvmContext(), 0));
        llvmRttiType = llvm::StructType::create(*module->getLlvmContext(), llvm::ArrayRef(members), string("@StructureMetadata"));
    }

    llvm::Constant* StructureMetadata::createRttiConstant(vector<llvm::Constant*>& args, CajetaStructure* structure, CajetaModule* module) {
        args.push_back(llvm::ConstantDataArray::getString(*module->getLlvmContext(),
                                                          structure->toCanonical(),
                                                          true));
        args.push_back(llvm::ConstantInt::get(llvmInt16Type, llvm::APInt(16, structure->getFields().size(), false)));
        vector<llvm::Constant*> fieldConstants;
        for (auto &field : structure->getFieldList()) {
            fieldConstants.push_back(createFieldConstant(field, module));
        }
        args.push_back(
            llvm::ConstantArray::get(
                llvm::ArrayType::get(llvmFieldType, fieldConstants.size()),
                llvm::ArrayRef<llvm::Constant*>(fieldConstants)
            )
        );

        // Count of Methods
        args.push_back(llvm::ConstantInt::get(llvmInt16Type, llvm::APInt(16, structure->getMethods().size(), false)));
        vector<llvm::Constant*> methodConstants;
        for (auto &method : structure->getMethodList()) {
            methodConstants.push_back(createMethodConstant(method, module));
        }
        return llvm::ConstantStruct::get(llvmRttiType, llvm::ArrayRef<llvm::Constant*>(args));
    }

    void StructureMetadata::buildInheritanceMethodMap(CajetaStructure* structure, map<string, Method*>& methodMap) {
        for (auto &superClass : structure->getSuperClasses()) {
            buildInheritanceMethodMap(superClass, methodMap);
        }
        for (auto &methodEntry : structure->getMethods()) {
            Method* method = methodEntry.second;
            auto itr = methodMap.find(method->toCanonical());
            if (itr != methodMap.end()) {
                method->setVirtualTableIndex((*itr).second->getVirtualTableIndex());
            } else {
                method->setVirtualTableIndex(virtualTableIndex++);
            }
            methodMap[methodEntry.second->toCanonical()] = methodEntry.second;
        }
    }

    /**
     * First, create virtual table, and store that in a global variable to be stored in the structure.  For the
     * virtual table, we will build the current generation's method array by recursively iterating from the root of
     * the hierarchy, building up a map of methods by canonical method name.  Each new method is awarded an ordinal
     * ID in the range.  When a subclass overrides the method, it assumes the same ordinal ID of its parent.
     *
     * This array is sorted by its ordinal, and then the resulting list is used to generate both the vtable type and
     * constant.
     *
     * Second, create the RTTI information structure, and store that as a global variable in the structure. The RTTI
     * structure does not include the vtable type, as that is unique per type, whereas the RTTI structure should be
     * shared by all structures.
     *
     * @param structure
     * @param module
     */
    void StructureMetadata::populateMetadataGlobals(CajetaStructure* structure, CajetaModule* module) {

        map<string, Method*> methodMap;
        virtualTableIndex = 0;
        buildInheritanceMethodMap(structure, methodMap);
        vector<Method*> sortedMethods;
        for (auto &methodEntry : methodMap) {
            sortedMethods.push_back(methodEntry.second);
        }

        sort(sortedMethods.begin(), sortedMethods.end(), MethodComparator());

        createVirtualTableType(structure, sortedMethods, module);
        llvm::Constant* virtualTableConstant = createVirtualTableConstant(structure->getVirtualTableType(), sortedMethods);
        llvm::GlobalVariable* virtualTableGlobal = (llvm::GlobalVariable*) module->getLlvmModule()->
                getOrInsertGlobal(structure->getVirtualTableType()->getStructName(), structure->getVirtualTableType());
        virtualTableGlobal->setInitializer(createVirtualTableConstant(structure->getVirtualTableType(), sortedMethods));
        structure->setVirtualTableGlobal(virtualTableGlobal);

        vector<llvm::Constant*> args;
        llvm::Constant* rttiConstant = createRttiConstant(args, structure, module);
        llvm::GlobalVariable* rttiGlobal = (llvm::GlobalVariable*) module->getLlvmModule()->getOrInsertGlobal(
                llvm::StringRef(string("@Rtti") + structure->toCanonical()), llvmRttiType);
        rttiGlobal->setInitializer(createRttiConstant(args, structure, module));
        structure->setRttiGlobal(rttiGlobal);
    }
} // cajeta