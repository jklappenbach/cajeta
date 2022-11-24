//
// Created by James Klappenbach on 11/20/22.
//

#pragma once

#include "cajeta/type/CajetaStructure.h"
#include "cajeta/compile/CajetaModule.h"

namespace cajeta {
    struct MethodComparator {
        // Compare 2 Player objects using name
        bool operator ()(Method* a, Method* b) {
            return a->getVirtualTableIndex() < b->getVirtualTableIndex();
        }
    };

    class StructureMetadata {
        int virtualTableIndex;

        llvm::Type* llvmInt16Type;
        llvm::StructType* llvmParameterType;
        llvm::StructType* llvmFieldType;
        llvm::StructType* llvmMethodType;
        llvm::StructType* llvmRttiType;

    public:
        /**
         * 1. Name
         * 2. Number of member variables
         * 3. Array of member variable entries
         *
         * @param structure
         * @param module
         */
        StructureMetadata(CajetaModule* module) {
            llvmInt16Type = llvm::IntegerType::getInt16Ty(*module->getLlvmContext());
            createParameterType(module);
            createFieldType(module);
            createMethodType(module);
            createRttiType(module);
            CajetaStructure::setRttiType(llvmRttiType);
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
        void populateMetadataGlobals(CajetaStructure* structure, CajetaModule* module);

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
        void createFieldType(CajetaModule* module);

        /**
         *
         * @param field
         * @param module
         * @return
         */
        llvm::Constant* createFieldConstant(StructureField* field, CajetaModule* module);

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
        void createParameterType(CajetaModule* module);

        /**
         *
         * @param parameter
         * @param module
         * @return
         */
        llvm::Constant* createParameterConstant(FormalParameter* parameter, CajetaModule* module);

        /**
         * 1. Method name
         * 2. Return type
         * 2. Number of parameters
         * 3. Array of parameters
         *
         * @param module
         */
        void createMethodType(CajetaModule* module);

        /**
         * TODO: Need to create a custom structure for the vtable for a given class, needs to use the FunctionType for each method
         * in the struct.
         * @param module
         */
        void createVirtualTableType(CajetaStructure* structure, vector<Method*> methods, CajetaModule* module);

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
        llvm::Constant* createMethodConstant(Method* method, CajetaModule* module);

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
        void createRttiType(CajetaModule* module);

        /**
         *
         * @param args
         * @param structure
         * @param module
         * @return
         */
        llvm::Constant* createRttiConstant(vector<llvm::Constant*>& args, CajetaStructure* structure, CajetaModule* module);

        /**
         *
         * @param structure
         * @param methodMap
         */
        void buildInheritanceMethodMap(CajetaStructure* structure, map<string, Method*>& methodMap);
    };
}// cajeta