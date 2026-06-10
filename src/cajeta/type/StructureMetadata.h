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
        llvm::Type* llvmInt32Type;
        llvm::Type* llvmInt64Type;
        llvm::Type* llvmPointerType;
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
            llvmInt32Type = llvm::IntegerType::getInt32Ty(*module->getLlvmContext());
            llvmInt64Type = llvm::IntegerType::getInt64Ty(*module->getLlvmContext());
            llvmPointerType = llvm::PointerType::get(*module->getLlvmContext(), 0);
            llvmRttiType = nullptr;
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

        // ---- Fixed-layout RTTI (REFL-1) -------------------------------------
        //
        // The emitted #RttiGlobal is a FIXED-offset header (same LLVM struct
        // type for every class) whose variable-length data — the type name,
        // the property/method/parameter descriptor tables, annotation and
        // parent-name lists — live in separately-emitted private globals,
        // referenced by pointer. This is what lets generic C natives in the
        // runtime (`__cajeta_rtti_*`) walk the metadata without per-class
        // offset knowledge. The C-side mirror structs (CajetaRtti / CajetaField
        // / CajetaMethod / CajetaParameter in cajeta_runtime.c) must stay in
        // lock-step with the struct shapes built here.
        //
        // Nothing read the old variable-length blob at runtime, so this layout
        // change is safe (AspectModel reads the compiler's CajetaClass model,
        // not the emitted global).

        // emit a private, null-terminated C string global; returns i8* to it.
        llvm::Constant* emitCString(const std::string& s);
        // emit a private array of i8* (one per string); returns ptr to the
        // array, or a null ptr constant when the list is empty.
        llvm::Constant* emitCStringArray(const vector<std::string>& strings);
        // OR the Modifier enum bits of a Modifiable into a packed int32.
        int32_t packModifiers(const std::set<Modifier>& modifiers);

        // Cached fixed descriptor struct types (built once, reused per class).
        llvm::StructType* getParameterStructType();
        llvm::StructType* getFieldStructType();
        llvm::StructType* getMethodStructType();
        llvm::StructType* getRttiStructType();
        // REFL-6b annotation argument descriptors. #AnnotationArgDesc carries a
        // single captured argument value (kind tag + scalar storage);
        // #AnnotationDesc is { name, argCount, args } — the shape the `annotations`
        // pointer in every owner descriptor now points at (was a bare i8* name
        // array in REFL-6a; the LLVM field type is still an opaque ptr, so no
        // descriptor-shape bump). Kept in lock-step with the C mirrors
        // CajetaAnnotationDesc / CajetaAnnotationArgDesc in cajeta_runtime.c.
        llvm::StructType* getAnnotationArgStructType();
        llvm::StructType* getAnnotationStructType();

        // Build the [M x #AnnotationArgDesc] table for one annotation's captured
        // arguments; null ptr constant when there are none.
        llvm::Constant* emitAnnotationArgArray(const vector<AnnotationArg>& args);
        // Build the [N x #AnnotationDesc] table for one annotatable owner. Names
        // come from `names` (the REFL-6a annotationList — order/count preserved);
        // argument values come from the matching AnnotationInstance (paired by
        // canonical name) when present. Parameter owners pass an empty
        // `instances` (their parse path captures names only), so their args
        // tables are empty but names still emit. Null ptr constant when empty.
        llvm::Constant* emitAnnotationArray(
            const list<QualifiedNamePtr>& names,
            const vector<AnnotationInstancePtr>& instances);

        // Build the per-class descriptor-table globals; return ptr to the
        // table (or null ptr constant when empty).
        llvm::Constant* emitFieldTable(CajetaClassPtr structure);
        llvm::Constant* emitMethodTable(CajetaClassPtr structure);
        // REFL-2C: per-class constructor descriptor table (#MethodDesc[] shape),
        // ordered by getReflectConstructorList — the index space the newInstance
        // adapter switches over.
        llvm::Constant* emitConstructorTable(CajetaClassPtr structure);
        llvm::Constant* emitParameterTable(MethodPtr method);

        llvm::Type* createAnnotationType(CajetaClassPtr structure);

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