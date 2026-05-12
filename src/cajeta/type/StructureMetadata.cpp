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

    // ---- RTTI: deferred ------------------------------------------------------
    //
    // The RTTI type/constant builders are intentionally stubs. The design
    // (version, type name, property/method/parent tables) is sketched in the
    // header's doc comment but the implementation hasn't landed — finishing
    // it is a separate piece of work. `populate` does NOT call into either
    // of these today; callers should not assume RTTI is available on a class.
    //
    // The previous draft of these methods left half-built code that would
    // crash (`createRttiConstant` dereferences `llvmRttiType`/`llvmPropertiesType`
    // which `createRttiType` never assigned). Marking explicitly stubbed
    // until someone picks it up.

    void StructureMetadata::createRttiType(CajetaClassPtr /*structure*/) {
        // TODO: build the RTTI struct type — version, name, properties,
        // methods, parents. See header doc comment for the layout sketch.
    }

    llvm::Constant* StructureMetadata::createRttiConstant(
            vector<llvm::Constant*>& /*args*/,
            CajetaClassPtr /*structure*/) {
        // TODO: pair with createRttiType once it actually builds the type.
        // Returning null is safe — no caller invokes this today.
        return nullptr;
    }

    void StructureMetadata::populate(CajetaClassPtr structure) {
        // Idempotent. Three states matter:
        //   1. Structure already has its vtable global → nothing to do.
        //   2. The LLVM module has the global but the structure forgot to
        //      record it (e.g. we built it in an earlier pass and the
        //      structure-side reference was lost) → re-link.
        //   3. Neither → build from scratch.
        if (structure->getVirtualTableGlobal() != nullptr) return;
        string globalName = structure->toCanonical() + string("#VTable");
        if (auto* existing = module->getLlvmModule()->getGlobalVariable(globalName)) {
            structure->setVirtualTableGlobal(existing);
            return;
        }

        // Build the type first so the global has somewhere to land. The
        // type-build uses the structure's virtualMethodList (populated by
        // CajetaClass::buildVirtualTable, which writeVirtualTable runs before
        // calling this method).
        createVirtualTableType(structure);
        auto* g = (llvm::GlobalVariable*) module->getLlvmModule()->
            getOrInsertGlobal(globalName, structure->getVirtualTableType());
        g->setInitializer(createVirtualTableConstant(structure));
        structure->setVirtualTableGlobal(g);

        // RTTI build is deferred — see `createRttiType` for the half-stubbed
        // shape; finishing it is its own piece of work.
    }

    // VTable layout: `{ i16 version, i16 count, ptr slot_0, ptr slot_1, ... }`.
    // Slots are function pointer values (opaque `ptr` under LLVM 18 opaque-
    // pointer mode); each slot holds the address of the virtual method whose
    // index matches the slot's position in `virtualMethodList`.
    //
    // The slot list comes from `structure->getVirtualMethodList()` — built by
    // `CajetaClass::buildVirtualTable` walking the hierarchy parent-first and
    // resolving overrides. The type and the constant share this same list,
    // so they're guaranteed to agree on arity and ordering.
    llvm::Type* StructureMetadata::createVirtualTableType(CajetaClassPtr structure) {
        const auto& slots = structure->getVirtualMethodList();
        llvm::Type* ptrTy = llvm::PointerType::get(*module->getLlvmContext(), 0);

        vector<llvm::Type*> members;
        members.reserve(2 + slots.size());
        // 0. Version
        members.push_back(llvmInt16Type);
        // 1. Number of slots
        members.push_back(llvmInt16Type);
        // 2..N+1. Function-pointer slots. We use opaque `ptr` rather than the
        // method's specific FunctionType — LLVM struct members can't be
        // FunctionType, only PointerType to a function.
        for (size_t i = 0; i < slots.size(); ++i) {
            members.push_back(ptrTy);
        }

        llvm::StructType* result = llvm::StructType::create(
            *module->getLlvmContext(),
            llvm::ArrayRef<llvm::Type*>(members),
            structure->toCanonical() + string("#VTable"));
        structure->setVirtualTableType(result);
        return result;
    }

    llvm::Constant* StructureMetadata::createVirtualTableConstant(CajetaClassPtr structure) {
        // Caller (`populate`) is responsible for having built the type. We do
        // NOT call createVirtualTableType here — doing so would re-clear and
        // re-build a struct that LLVM has already created and possibly handed
        // out references to.
        const auto& slots = structure->getVirtualMethodList();
        vector<llvm::Constant*> args;
        args.reserve(2 + slots.size());

        // 0. Version
        args.push_back(llvm::ConstantInt::get(
            llvmInt16Type, llvm::APInt(16, 0, false)));
        // 1. Slot count
        args.push_back(llvm::ConstantInt::get(
            llvmInt16Type, llvm::APInt(16, slots.size(), false)));
        // 2..N+1. Function pointers — `llvm::Function*` is itself a
        // `llvm::Constant*`, so it slots in directly as a pointer constant.
        for (auto& method : slots) {
            args.push_back(method->getLlvmFunction());
        }

        return llvm::ConstantStruct::get(structure->getVirtualTableType(),
            llvm::ArrayRef<llvm::Constant*>(args));
    }
} // code