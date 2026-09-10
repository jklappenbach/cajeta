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

    /** Emits one class's metadata: the fixed-layout #RttiGlobal header, the
     *  private globals its descriptor tables point at, and the vtable global. */
    class StructureMetadata {
        CajetaModulePtr module;
        llvm::Type* llvmInt16Type;
        llvm::Type* llvmInt8Type;
        llvm::Type* llvmInt32Type;
        llvm::Type* llvmInt64Type;
        llvm::Type* llvmPointerType;
        llvm::StructType* llvmRttiType;
        // Vtable slots that had no llvm::Function when the constant was built:
        // `ptr null` is emitted so the constant completes (the global is
        // forward-declared and cyclic), and populate throws once it is whole.
        vector<std::string> vtableNullSlots;

    public:
        /** Cache the LLVM types used throughout emission. */
        StructureMetadata(CajetaModulePtr module) {
            this->module = module;
            llvmInt16Type = llvm::IntegerType::getInt16Ty(*module->getLlvmContext());
            llvmInt8Type = llvm::IntegerType::getInt8Ty(*module->getLlvmContext());
            llvmInt32Type = llvm::IntegerType::getInt32Ty(*module->getLlvmContext());
            llvmInt64Type = llvm::IntegerType::getInt64Ty(*module->getLlvmContext());
            llvmPointerType = llvm::PointerType::get(*module->getLlvmContext(), 0);
            llvmRttiType = nullptr;
        }

        /** Emit the vtable global — ordinals assigned by walking down from the
         *  root of the hierarchy, an override taking its parent's slot — and then
         *  the RTTI global, which excludes the per-type vtable type. */
        void populate(CajetaClassPtr structure);

    private:

        // ---- Fixed-layout RTTI (REFL-1) -------------------------------------
        // #RttiGlobal is a fixed-offset header — one LLVM struct type for every
        // class — whose variable-length data lives in private globals reached by
        // pointer. The C mirrors in cajeta_runtime.c stay in lock-step with it.

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
        // REFL-6b annotation argument descriptors: #AnnotationArgDesc is one
        // captured value (kind tag + scalar), #AnnotationDesc is { name, argCount,
        // args }. Mirrors CajetaAnnotationDesc / ...ArgDesc in cajeta_runtime.c.
        llvm::StructType* getAnnotationArgStructType();
        llvm::StructType* getAnnotationStructType();

        // Build the [M x #AnnotationArgDesc] table for one annotation's captured
        // arguments; null ptr constant when there are none.
        llvm::Constant* emitAnnotationArgArray(const vector<AnnotationArg>& args);
        // REFL-7 template reflection. #TemplateParamDesc mirrors a declared
        // template parameter (name + bounds + non-type info). Kept in lock-step
        // with CajetaTemplateParamDesc in cajeta_runtime.c.
        llvm::StructType* getTemplateParamStructType();
        // Build the [N x #TemplateParamDesc] table from a class's declared
        // template parameters; null ptr constant when it declares none.
        llvm::Constant* emitTemplateParamTable(CajetaClassPtr structure);
        // Build the [N x i8*] of an instantiation's concrete template-argument
        // canonical names; null ptr constant when it has none.
        llvm::Constant* emitTemplateArgArray(CajetaClassPtr structure);

        // Build the [N x #AnnotationDesc] table for one annotatable owner: names
        // from `names`, order and count preserved; argument values paired by
        // canonical name out of `instances`. Null ptr constant when empty.
        llvm::Constant* emitAnnotationArray(
            const list<QualifiedNamePtr>& names,
            const vector<AnnotationInstancePtr>& instances);

        // Build the per-class descriptor-table globals; return ptr to the
        // table (or null ptr constant when empty).
        llvm::Constant* emitFieldTable(CajetaClassPtr structure);
        llvm::Constant* emitMethodTable(CajetaClassPtr structure);
        // REFL-2C per-class constructor table (#MethodDesc[] shape), ordered by
        // getReflectConstructorList — the index space newInstance switches over.
        llvm::Constant* emitConstructorTable(CajetaClassPtr structure);
        llvm::Constant* emitParameterTable(MethodPtr method);

        llvm::Type* createAnnotationType(CajetaClassPtr structure);

        /** Build the class's vtable LLVM type.
         *  TODO: use each method's FunctionType instead of opaque pointers. */
        llvm::Type* createVirtualTableType(CajetaClassPtr structure);

        /** Build the vtable constant; a slot with no function records in vtableNullSlots. */
        llvm::Constant* createVirtualTableConstant(CajetaClassPtr structure);

        /** Build and cache the fixed #Rtti struct type: vtable pointer, type
         *  name, and the counted field, method and parent descriptor tables. */
        void createRttiType(CajetaClassPtr structure);

        /** Assemble the #RttiGlobal initializer from the already-built `args`. */
        llvm::Constant* createRttiConstant(vector<llvm::Constant*>& args, CajetaClassPtr structure);
    };
}// code