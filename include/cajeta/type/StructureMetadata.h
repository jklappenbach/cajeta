//
// Created by James Klappenbach on 11/20/22.
//

#pragma once

#include "cajeta/type/CajetaStructure.h"
#include "cajeta/compile/CajetaModule.h"

#define INTERNAL_PREFIX         string("#")
#define INTERNAL_NAME(str)      INTERNAL_PREFIX + str

namespace cajeta {
    struct MethodComparator {
        bool operator()(Method* a, Method* b) {
            return a->getVirtualTableIndex() < b->getVirtualTableIndex();
        }
    };

    class StructureMetadata {
        int virtualTableIndex;
        CajetaModule* module;
        llvm::Type* llvmInt16Type;
        llvm::Type* llvmInt8Type;
        llvm::StructType* llvmPropertiesType;
        vector<llvm::StructType*> llvmParametersType;
        llvm::StructType* llvmRttiType;

    public:
        /**
         * 1. Get common int16 type
         * 2. Create Parameter Type
         * 3. Create Field Type
         * 2. Create Method Type
         * 3. Create RTTI Type
         * 4. Set CajetaStructure's static variable for the type
         *
         * @param module
         */
        StructureMetadata(CajetaModule* module) {
            this->module = module;
            llvmInt16Type = llvm::IntegerType::getInt16Ty(*module->getLlvmContext());
            llvmInt8Type = llvm::IntegerType::getInt8Ty(*module->getLlvmContext());
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
        void populateMetadataGlobals(CajetaStructure* structure);

    private:
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
        llvm::Type* createPropertyType(CajetaStructure* structure, ClassProperty* property);

        /**
         *
         * @param field
         * @param module
         * @return
         */
        llvm::Constant* createPropertyConstant(ClassProperty* property, llvm::StructType* propertyType);

        /**
         * 1. Parameter Name (string, array)
         * 2. Parameter Type (string, array)
         * 3. Modifier Count (int8)
         * 4. Modifiers (int8, array)
         * 5. Annotation Count (int8)
         * 6. Structure annotations (array of strings)
         *
         * @param module
         */
        llvm::StructType* createParameterType(FormalParameter* parameter);

        /**
         * 1. Parameter name
         * 2. Type canonical
         * 3. Modifier count
         * 4. Array of modifiers
         * 5. Annotation count
         * 6. Structure of annotations
         *
         * @param parameter
         * @return
         */
        llvm::Constant* createParameterConstant(FormalParameter* parameter, llvm::StructType* parameterType);

        /**
         * 1. Method name
         * 2. Return type
         * 2. Number of parameters
         * 3. Array of parameters
         *
         * @param module
         */
        llvm::StructType* createMethodType(Method* method);

        /**
         * TODO: Need to create a custom structure for the vtable for a given class, needs to use the FunctionType for each method
         * in the struct.
         * @param module
         */
        void createVirtualTableType(CajetaStructure* structure, vector<Method*> methods);

        /**
         *
         * @param llvmVirtualTableType
         * @param methods
         * @return
         */
        llvm::Constant* createVirtualTableConstant(llvm::StructType* llvmVirtualTableType, vector<Method*>& methods);

        /**
         *
         * @param method
         * @param module
         * @return
         */
        llvm::Constant* createMethodConstant(Method* method, llvm::StructType* methodType);

        /**
         * 1. Array of Function pointers (vtable)
         * 2. Array of FunctionType pointers
         * 3. Type name
         * 4. Size of properties
         * 5. Array of properties
         * 6. Size of class method set
         * 7. Array of methods
         * 8. Size of parent structures
         * 9. Pointers to parent structures (array)
         *
         * @param module
         */
        void createRttiType(CajetaStructure* structure);

        /**
         *
         * @param args
         * @param structure
         * @param module
         * @return
         */
        llvm::Constant* createRttiConstant(vector<llvm::Constant*>& args, CajetaStructure* structure);

        /**
         *
         * @param structure
         * @param methodMap
         */
        void buildInheritanceMethodMap(CajetaStructure* structure, map<string, Method*>& methodMap);
    };
}// cajeta