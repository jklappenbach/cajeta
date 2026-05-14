//
// Created by James Klappenbach on 11/20/22.
//

#include "StructureMetadata.h"

#include <cstdint>

namespace cajeta {

    // FNV-1a 64-bit; matches the runtime's __cajeta_signature_hash so the
    // compile-time hash of a method's canonical signature equals the runtime
    // lookup hash at dispatch time.
    static int64_t signatureHash(const std::string& s) {
        uint64_t h = 0xcbf29ce484222325ULL;
        for (unsigned char c : s) {
            h ^= c;
            h *= 0x100000001b3ULL;
        }
        return (int64_t) h;
    }

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

        args.push_back(llvm::ConstantStruct::get((llvm::StructType*) parameterType->getTypeAtIndex(5),
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
        // Each member's size has to match what `createMethodConstant` will
        // emit — the constant uses `toCanonical()` (the canonical signature)
        // for the name slot and `ConstantDataArray::getString(..., /*null=*/true)`
        // for both string fields, so the types here need to include the
        // null terminator.
        vector<llvm::Type*> members;
        members.push_back(llvm::ArrayType::get(llvmInt8Type, method->toCanonical().size() + 1));
        members.push_back(llvm::ArrayType::get(llvmInt8Type, method->getReturnType()->toCanonical().size() + 1));
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

    // ---- RTTI: per-class metadata blob --------------------------------------
    //
    // Layout (one unique LLVM struct type per class because string members
    // are fixed-size `[N x i8]` and N depends on the class's name length):
    //
    //   {
    //     i64  allocationSize,         // sizeof(instance)
    //     [N x i8] typeName,           // canonical name, null-terminated
    //     i8   classAnnotationCount,
    //     { [ann1 x i8], [ann2 x i8], ... } classAnnotations,   // strings
    //     i16  propertyCount,
    //     { PropertyType_0, PropertyType_1, ... } properties,
    //     i16  methodCount,
    //     { MethodType_0, MethodType_1, ... } methods,
    //     i16  parentCount,
    //     { [n1 x i8], [n2 x i8], ... } parentNames,
    //     ptr  vtable
    //   }
    //
    // Each PropertyType / MethodType is itself a unique struct (per
    // createPropertyType / createMethodType) so a class with 3 properties
    // produces 4 named LLVM types: the RTTI shell plus one per property.
    // That's expected — variable-length string members force per-instance
    // typing, and the LLVM module's type table dedupes by structural
    // identity anyway.
    //
    // What's NOT here yet:
    //   - Annotation arguments (Annotatable today carries only names).
    //   - "imports" (used elsewhere; not a structural property of the type).
    //   - Recursive Type descriptors for parents (using canonical names
    //     instead — cheaper, and the runtime can look up the parent's RTTI
    //     global by name when it needs deeper info).

    void StructureMetadata::createRttiType(CajetaClassPtr structure) {
        auto& ctx = *module->getLlvmContext();
        llvm::Type* i64Ty = llvm::IntegerType::getInt64Ty(ctx);
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);

        vector<llvm::Type*> members;
        // 0. allocationSize
        members.push_back(i64Ty);
        // 1. typeName: i8[len+1]
        members.push_back(llvm::ArrayType::get(llvmInt8Type,
            structure->toCanonical().size() + 1));
        // 2. classAnnotationCount
        members.push_back(llvmInt8Type);
        // 3. classAnnotations
        vector<llvm::Type*> classAnnotationTypes;
        for (auto& qName : structure->getAnnotationList()) {
            classAnnotationTypes.push_back(llvm::ArrayType::get(llvmInt8Type,
                qName->toCanonical().size() + 1));
        }
        members.push_back(llvm::StructType::get(ctx,
            llvm::ArrayRef<llvm::Type*>(classAnnotationTypes)));
        // 4. propertyCount
        members.push_back(llvmInt16Type);
        // 5. properties
        vector<llvm::Type*> propertyTypes;
        for (auto& property : structure->getPropertyList()) {
            propertyTypes.push_back(createPropertyType(structure, property));
        }
        llvmPropertiesType = llvm::StructType::get(ctx,
            llvm::ArrayRef<llvm::Type*>(propertyTypes));
        members.push_back(llvmPropertiesType);
        // 6. methodCount
        members.push_back(llvmInt16Type);
        // 7. methods
        vector<llvm::Type*> methodTypes;
        for (auto& method : structure->getMethodList()) {
            methodTypes.push_back(createMethodType(method));
        }
        members.push_back(llvm::StructType::get(ctx,
            llvm::ArrayRef<llvm::Type*>(methodTypes)));
        // 8. parentCount
        members.push_back(llvmInt16Type);
        // 9. parentNames: struct of canonical strings
        vector<llvm::Type*> parentNameTypes;
        for (auto& parent : structure->getSuperClasses()) {
            parentNameTypes.push_back(llvm::ArrayType::get(llvmInt8Type,
                parent->toCanonical().size() + 1));
        }
        members.push_back(llvm::StructType::get(ctx,
            llvm::ArrayRef<llvm::Type*>(parentNameTypes)));
        // 10. vtable
        members.push_back(ptrTy);

        llvm::StructType* result = llvm::StructType::create(ctx,
            llvm::ArrayRef<llvm::Type*>(members),
            structure->toCanonical() + string("#RttiType"));
        llvmRttiType = result;
        structure->setRttiType(result);
    }

    llvm::Constant* StructureMetadata::createRttiConstant(
            vector<llvm::Constant*>& args,
            CajetaClassPtr structure) {
        auto& ctx = *module->getLlvmContext();
        llvm::Type* i64Ty = llvm::IntegerType::getInt64Ty(ctx);

        // 0. allocationSize
        uint64_t allocSize = 0;
        if (structure->getLlvmType() && llvm::isa<llvm::StructType>(structure->getLlvmType())) {
            allocSize = module->getLlvmModule()->getDataLayout()
                .getTypeAllocSize(structure->getLlvmType());
        }
        args.push_back(llvm::ConstantInt::get(i64Ty,
            llvm::APInt(64, allocSize, false)));
        // 1. typeName
        args.push_back(llvm::ConstantDataArray::getString(ctx,
            structure->toCanonical(), true));
        // 2. classAnnotationCount
        args.push_back(llvm::ConstantInt::get(llvmInt8Type,
            llvm::APInt(8, structure->getAnnotationList().size(), false)));
        // 3. classAnnotations
        vector<llvm::Constant*> classAnnotationConstants;
        for (auto& qName : structure->getAnnotationList()) {
            classAnnotationConstants.push_back(
                llvm::ConstantDataArray::getString(ctx, qName->toCanonical(), true));
        }
        auto* classAnnotationsTy = llvm::cast<llvm::StructType>(
            llvmRttiType->getTypeAtIndex(3));
        args.push_back(llvm::ConstantStruct::get(classAnnotationsTy,
            llvm::ArrayRef<llvm::Constant*>(classAnnotationConstants)));
        // 4. propertyCount
        args.push_back(llvm::ConstantInt::get(llvmInt16Type,
            llvm::APInt(16, structure->getPropertyList().size(), false)));
        // 5. properties
        vector<llvm::Constant*> propertyConstants;
        unsigned propIdx = 0;
        for (auto& property : structure->getPropertyList()) {
            auto* propTy = llvm::cast<llvm::StructType>(
                llvmPropertiesType->getTypeAtIndex(propIdx++));
            propertyConstants.push_back(createPropertyConstant(property, propTy));
        }
        args.push_back(llvm::ConstantStruct::get(llvmPropertiesType,
            llvm::ArrayRef<llvm::Constant*>(propertyConstants)));
        // 6. methodCount
        args.push_back(llvm::ConstantInt::get(llvmInt16Type,
            llvm::APInt(16, structure->getMethodList().size(), false)));
        // 7. methods
        auto* methodsTy = llvm::cast<llvm::StructType>(
            llvmRttiType->getTypeAtIndex(7));
        vector<llvm::Constant*> methodConstants;
        unsigned methIdx = 0;
        for (auto& method : structure->getMethodList()) {
            auto* methTy = llvm::cast<llvm::StructType>(
                methodsTy->getTypeAtIndex(methIdx++));
            methodConstants.push_back(createMethodConstant(method, methTy));
        }
        args.push_back(llvm::ConstantStruct::get(methodsTy,
            llvm::ArrayRef<llvm::Constant*>(methodConstants)));
        // 8. parentCount
        args.push_back(llvm::ConstantInt::get(llvmInt16Type,
            llvm::APInt(16, structure->getSuperClasses().size(), false)));
        // 9. parentNames
        vector<llvm::Constant*> parentNameConstants;
        for (auto& parent : structure->getSuperClasses()) {
            parentNameConstants.push_back(
                llvm::ConstantDataArray::getString(ctx, parent->toCanonical(), true));
        }
        auto* parentNamesTy = llvm::cast<llvm::StructType>(
            llvmRttiType->getTypeAtIndex(9));
        args.push_back(llvm::ConstantStruct::get(parentNamesTy,
            llvm::ArrayRef<llvm::Constant*>(parentNameConstants)));
        // 10. vtable
        llvm::Constant* vtable = structure->getVirtualTableGlobal();
        if (!vtable) {
            vtable = llvm::ConstantPointerNull::get(
                llvm::PointerType::get(ctx, 0));
        }
        args.push_back(vtable);

        return llvm::ConstantStruct::get(llvmRttiType,
            llvm::ArrayRef<llvm::Constant*>(args));
    }

    void StructureMetadata::populate(CajetaClassPtr structure) {
        // Idempotent. Builds two globals per structure: the vtable and the
        // RTTI blob. Both bail-out checks are independent so re-entry after a
        // partial build (only one of the two emitted) recovers cleanly.

        // --- vtable -----------------------------------------------------------
        if (structure->getVirtualTableGlobal() == nullptr) {
            string vtableName = structure->toCanonical() + string("#VTable");
            if (auto* existing = module->getLlvmModule()->getGlobalVariable(vtableName)) {
                structure->setVirtualTableGlobal(existing);
            } else {
                // Build the type first so the global has somewhere to land.
                // The type-build uses virtualMethodList (populated by
                // CajetaClass::buildVirtualTable, which writeVirtualTable
                // runs before calling this method).
                createVirtualTableType(structure);
                auto* g = (llvm::GlobalVariable*) module->getLlvmModule()->
                    getOrInsertGlobal(vtableName, structure->getVirtualTableType());
                g->setInitializer(createVirtualTableConstant(structure));
                structure->setVirtualTableGlobal(g);
            }
        }

        // --- RTTI -------------------------------------------------------------
        if (structure->getRttiGlobal() == nullptr) {
            string rttiName = structure->toCanonical() + string("#RttiGlobal");
            if (auto* existing = module->getLlvmModule()->getGlobalVariable(rttiName)) {
                structure->setRttiGlobal(existing);
            } else {
                createRttiType(structure);
                auto* g = (llvm::GlobalVariable*) module->getLlvmModule()->
                    getOrInsertGlobal(rttiName, structure->getRttiType());
                vector<llvm::Constant*> args;
                g->setInitializer(createRttiConstant(args, structure));
                structure->setRttiGlobal(g);
            }
        }
    }

    // VTable layout:
    //   { i16 version, i16 count, ptr parent_vtable,
    //     [count x { i64 hash, ptr fn }] entries }
    //
    // Entries are sorted by hash (FNV-1a of method->toCanonical(false)) so
    // dispatch is a binary search via __cajeta_vtable_lookup. Hash-based
    // addressing means each method has a stable identity regardless of how
    // many bases the class inherits from — multiple inheritance just works,
    // no thunks or per-base offset trickery.
    //
    // parent_vtable points at the direct superclass's vtable global (or
    // NULL for the root). Used by __cajeta_is_unrecoverable to walk up the
    // hierarchy at runtime, checking if any ancestor matches the
    // UnrecoverableException sentinel — and by future RTTI helpers that
    // need arbitrary up-the-chain queries.
    //
    // Layout note: i16/i16 (4 bytes) + 4 bytes pad + ptr (8 bytes) puts
    // the entries array at byte offset 16. The runtime helper's pointer
    // arithmetic uses that constant.
    llvm::Type* StructureMetadata::createVirtualTableType(CajetaClassPtr structure) {
        auto& ctx = *module->getLlvmContext();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::Type* i64Ty = llvm::IntegerType::getInt64Ty(ctx);

        const auto& slots = structure->getVirtualMethodList();

        // Entry: { i64 hash, ptr fn }. Anonymous (literal) struct so the LLVM
        // module's type table dedupes; doesn't need a name of its own.
        llvm::StructType* entryTy = llvm::StructType::get(ctx, {i64Ty, ptrTy});
        llvm::ArrayType* entriesTy = llvm::ArrayType::get(entryTy, slots.size());

        vector<llvm::Type*> members{
            llvmInt16Type,    // 0. version
            llvmInt16Type,    // 1. count
            ptrTy,            // 2. parent_vtable
            entriesTy,        // 3. entries
        };
        llvm::StructType* result = llvm::StructType::create(ctx,
            llvm::ArrayRef<llvm::Type*>(members),
            structure->toCanonical() + string("#VTable"));
        structure->setVirtualTableType(result);
        return result;
    }

    llvm::Constant* StructureMetadata::createVirtualTableConstant(CajetaClassPtr structure) {
        // Caller (populate) has already built the type — we do NOT recreate
        // it here. The slot list lives on the class (built by
        // CajetaClass::buildVirtualTable, already hash-sorted).
        auto& ctx = *module->getLlvmContext();
        llvm::Type* i64Ty = llvm::IntegerType::getInt64Ty(ctx);
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        const auto& slots = structure->getVirtualMethodList();

        // Layout — index 3 now holds entries (after version/count/parent_vtable).
        llvm::ArrayType* entriesArrTy = llvm::cast<llvm::ArrayType>(
            structure->getVirtualTableType()->getTypeAtIndex(3));
        llvm::StructType* entryTy = llvm::cast<llvm::StructType>(
            entriesArrTy->getElementType());

        // Use the lock-step hash list from buildVirtualTable rather than
        // recomputing — interface entries store the implementing class's
        // Method (so getLlvmFunction returns the concrete function) but
        // the hash needs to be the *interface* method's canonical, which
        // recomputing from `method` would lose.
        const auto& slotHashes = structure->getVirtualSlotHashList();
        vector<llvm::Constant*> entryConstants;
        entryConstants.reserve(slots.size());
        size_t hashIdx = 0;
        for (auto& method : slots) {
            int64_t hash = (hashIdx < slotHashes.size())
                ? slotHashes[hashIdx]
                : signatureHash(method->toCanonical(/*labeled=*/false));
            entryConstants.push_back(llvm::ConstantStruct::get(entryTy, {
                llvm::ConstantInt::get(i64Ty, llvm::APInt(64, (uint64_t) hash, false)),
                method->getLlvmFunction(),
            }));
            ++hashIdx;
        }
        llvm::Constant* entriesArr = llvm::ConstantArray::get(
            entriesArrTy, llvm::ArrayRef<llvm::Constant*>(entryConstants));

        // parent_vtable: pointer to the direct superclass's vtable global.
        // NULL when the class has no parent (e.g. Throwable). For multi-
        // parent (not supported today), we'd need a more elaborate
        // representation; the first superclass wins for now.
        llvm::Constant* parentVtable =
            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
        const auto& parents = structure->getSuperClasses();
        if (!parents.empty()) {
            auto firstParent = parents.front();
            if (llvm::GlobalVariable* pv = firstParent->getVirtualTableGlobal()) {
                parentVtable = pv;
            }
        }

        vector<llvm::Constant*> args{
            llvm::ConstantInt::get(llvmInt16Type, llvm::APInt(16, 0, false)),
            llvm::ConstantInt::get(llvmInt16Type,
                llvm::APInt(16, slots.size(), false)),
            parentVtable,
            entriesArr,
        };
        return llvm::ConstantStruct::get(structure->getVirtualTableType(),
            llvm::ArrayRef<llvm::Constant*>(args));
    }
} // code