//
// Created by James Klappenbach on 11/20/22.
//

#pragma once

#include "CajetaClass.h"
#include "../compile/CajetaModule.h"

#define INTERNAL_PREFIX         string("#")
#define INTERNAL_NAME(str)      INTERNAL_PREFIX + str

namespace cajeta {
    struct MethodComparator {
        bool operator()(MethodPtr a, MethodPtr b) {
            return a->getVirtualTableIndex() < b->getVirtualTableIndex();
        }
    };

    /**
     *  ### Metadata Structures
     *  String
     *      length: uint16
     *      data: uint8[]
     *  Type
     *      name: String
     *      parentCount: uint8
     *      parents: Type[]
     *  Parameter
     *      type: Type
     *      name: String
     *      annotationCount: uint8
     *      annotations: Type[]
     *  Argument
     *      name:String
     *      value:String
     *  AnnotationInstance
     *      type:Type
     *      argumentCount:int8
     *      arguments:Argument[]
     *  Import
     *      type: Type
     *  Property
     *      type: Type
     *      name: String
     *      annotationCount: uint8
     *      annotations: AnnotationInstances[]
     *  Method
     *      name: String
     *      returnType: Type
     *      annotationCount: uint8
     *      annotationInstances: AnnotationInstances[]
     *      parameterCount: uint8
     *      parameters: Parameter[]
     *  Function
     *      type: [Local|Remote]
     *      name: String
     *      pointer: Function*
     *  VirtualTable
     *      functionCount: uint8
     *      functions: Function[]
     *
     * #### Class Metadata
     *
     * 1. allocationSize: uint64
     * 2. type: Type
     * 3. annotationCount: uint8
     * 4. AnnotationInstance[]
     * 5. importCount: uint16
     * 6. imports: Import[]
     * 7. propertyCount: uint8
     * 8. properties: Property[]
     * 9. methodCount: uint8
     * 10. methods: Method[]
     * 11. virtualTable: VirtualTable
     */
    class StructureMetadata {
        CajetaModulePtr module;
        llvm::Type* llvmInt16Type;
        llvm::Type* llvmInt8Type;
        llvm::Type* llvmPointerType;
        llvm::StructType* llvmPropertiesType;
        vector<llvm::StructType*> llvmParametersType;
        llvm::StructType* llvmRttiType;

    public:
        /**
         * Cache the default types that we'll use frequently.
         *
         * @param module
         */
        StructureMetadata(CajetaModulePtr module) {
            this->module = module;
            llvmInt16Type = llvm::IntegerType::getInt16Ty(*module->getLlvmContext());
            llvmInt8Type = llvm::IntegerType::getInt8Ty(*module->getLlvmContext());
            llvmPointerType = llvm::PointerType::getVoidTy(*module->getLlvmContext());
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
        void populate(CajetaClassPtr structure);

    private:

        llvm::Type* createAnnotationType(CajetaClassPtr structure);
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
        llvm::Type* createPropertyType(CajetaClassPtr structure, StructurePropertyPtr property);

        /**
         *
         * @param field
         * @param module
         * @return
         */
        llvm::Constant* createPropertyConstant(StructurePropertyPtr property, llvm::StructType* propertyType);

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
        llvm::StructType* createParameterType(FormalParameterPtr parameter);

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
        llvm::Constant* createParameterConstant(FormalParameterPtr parameter, llvm::StructType* parameterType);

        /**
         * 1. name: String
         * 2. returnType: Type
         * 3. annotationCount: uint8
         * 4. annotationInstances: AnnotationInstances[]
         * 5. parameterCount: uint8
         * 6. parameters: Parameter[]
         *
         * @param module
         */
        llvm::StructType* createMethodType(MethodPtr method);

        /**
         *
         * @param method
         * @param module
         * @return
         */
        llvm::Constant* createMethodConstant(MethodPtr method, llvm::StructType* methodType);


        /**
         * TODO: Need to create a custom structure for the vtable for a given class, needs to use the FunctionType for each method
         * in the struct.
         * @param module
         */
        llvm::Type* createVirtualTableType(CajetaClassPtr structure);

        /**
         *
         * @param llvmVirtualTableType
         * @param methods
         * @return
         */
        llvm::Constant* createVirtualTableConstant(CajetaClassPtr structure);

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
        void createRttiType(CajetaClassPtr structure);

        /**
         *
         * @param args
         * @param structure
         * @param module
         * @return
         */
        llvm::Constant* createRttiConstant(vector<llvm::Constant*>& args, CajetaClassPtr structure);
    };
}// code