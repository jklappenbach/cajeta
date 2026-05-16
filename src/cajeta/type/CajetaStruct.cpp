//
// CajetaStruct — stack value aggregate. See Structs.md.
//
// S6.1: lay out the LLVM struct body for primitive-typed fields, register
// the type in the canonical map, and accept declarations + locals. Subsequent
// sub-tasks extend the allowed field set (S6.3 class refs), add the aggregate
// initializer parser path (S6.2), drop chain (S6.4), borrow tracking (S6.5),
// and reject variable-tail fields explicitly (S6.6). Inline composition into
// classes is S7; methods are S8; interface dispatch is S9-S11.
//

#include "CajetaStruct.h"
#include "CajetaArray.h"
#include "CajetaView.h"
#include "CajetaInterface.h"
#include "../compile/CajetaModule.h"
#include "../method/Method.h"
#include "../error/Exception.h"
#include <algorithm>
#include <llvm/IR/IRBuilder.h>

namespace cajeta {

    void CajetaStruct::generatePrototype() {
        string canonical = qName->toCanonical();

        // Register the type up front so field-type validation below can
        // recognize self-reference (recursive struct rejection) and so
        // any method prototypes generated at the end of this function
        // can resolve canonical names that already include this struct.
        llvmType = CajetaType::getOrCreateLlvmType(module->getLlvmContext(), canonical);
        typeMap[TypeKey(llvmType)] = shared_from_this();
        canonicalMap[canonical] = static_pointer_cast<CajetaType>(shared_from_this());
        canonicalMap[qName->getTypeName()] = static_pointer_cast<CajetaType>(shared_from_this());
        typeFlags = STRUCT_FLAG | USER_DEFINED_FLAG;

        // Field validation. v1 (S6.1) only accepts primitive-typed fields;
        // class-ref fields land in S6.3, inline structs in a later session.
        // Reject explicitly with a message pointing at the rollout doc so
        // the error mode is obvious during the rollout window.
        vector<llvm::Type*> llvmMembers;
        llvmMembers.reserve(propertyList.size());
        for (auto& property : propertyList) {
            auto fieldType = property->getType();

            // Direct recursion guard: a struct containing itself has
            // infinite size. Matches the same shape view uses; transitive
            // cycles (A holds B, B holds A) are deferred until composition
            // is exercised by real code. Class refs land in S6.3, which
            // has shipped — use one if you need recursive structure.
            if (fieldType && fieldType->getQName()
                    && fieldType->getQName()->toCanonical() == canonical) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                    "struct '%s' has field '%s' of its own type; recursive structs "
                    "are forbidden (would have infinite size). Use a class reference "
                    "if you need recursive structure.",
                    canonical.c_str(), property->getName().c_str());
                throw Exception(buf, "CAJETA_ERROR_STRUCT_RECURSIVE");
            }

            // S6.6 — reject variable-tail fields. In view declarations,
            // `T[]` (and `String`) fields lay out inline as a length-prefix
            // followed by data bytes — the variable-tail encoding. That
            // shape needs a backing buffer and a fixed framing, which is
            // a view's whole job. Structs are fixed-size stack aggregates;
            // every field must have a compile-time-known size (Structs.md
            // § Allowed field types). `T[]` in a struct would have to mean
            // a heap-allocated array reference — that's exactly what a
            // class-ref field gives you, so use a wrapper class instead.
            //
            // `String` IS allowed as a struct field (a class-ref slot per
            // S6.3) — it carries a pointer to a heap String whose internal
            // layout is the String's own concern.
            if (dynamic_pointer_cast<CajetaArray>(fieldType)) {
                char buf[320];
                snprintf(buf, sizeof(buf),
                    "struct '%s' field '%s' has array type T[] — that's a "
                    "variable-tail shape only valid in views (where it lays "
                    "out as length-prefix + data bytes). Structs require "
                    "fixed-size fields; for a heap array, wrap it in a "
                    "class and hold a class reference.",
                    canonical.c_str(), property->getName().c_str());
                throw Exception(buf, "CAJETA_ERROR_STRUCT_FIELD_TYPE");
            }
            // Views as struct fields are deferred — viable in principle
            // (a view is itself an aggregate) but unexercised; reject for
            // now. Independent from the variable-tail rejection above:
            // even a view with all fixed-size fields is rejected here.
            if (dynamic_pointer_cast<CajetaView>(fieldType)) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                    "struct '%s' field '%s' has view type — view-as-struct-field "
                    "is not supported in v1.",
                    canonical.c_str(), property->getName().c_str());
                throw Exception(buf, "CAJETA_ERROR_STRUCT_FIELD_TYPE");
            }

            // Allowed field types:
            //   - primitives (inline LLVM type)
            //   - nested CajetaStruct (inline LLVM struct type)
            //   - plain CajetaClass refs (8-byte pointer slot per Structs.md
            //     § "Class references occupy a single pointer-width slot")
            // Interfaces land in S10 (fat pointer, 24 bytes); rejected here
            // with a separate error ID so the message points at the right
            // future work.
            // Interface detection uses the isInterface flag rather than
            // dynamic_pointer_cast<CajetaInterface> because the visitor
            // stores interfaces as plain CajetaClass with isInterface()=true
            // (see CajetaLlvmVisitor::visitInterfaceDeclaration).
            bool isPrimitive = fieldType && (fieldType->getTypeFlags() & PRIMITIVE_FLAG);
            bool isNestedStruct = dynamic_pointer_cast<CajetaStruct>(fieldType) != nullptr;
            auto fieldClass = dynamic_pointer_cast<CajetaClass>(fieldType);
            bool isInterface = fieldClass && fieldClass->isInterface();
            bool isClassRef = fieldClass != nullptr
                && !dynamic_pointer_cast<CajetaAggregate>(fieldType)
                && !isInterface;
            if (isInterface) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                    "struct '%s' field '%s' has interface type — interface "
                    "fields are S10 of the rollout (tagged fat pointer support).",
                    canonical.c_str(), property->getName().c_str());
                throw Exception(buf, "CAJETA_ERROR_STRUCT_FIELD_TYPE");
            }
            if (!isPrimitive && !isNestedStruct && !isClassRef) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                    "struct '%s' field '%s' has unsupported field type",
                    canonical.c_str(), property->getName().c_str());
                throw Exception(buf, "CAJETA_ERROR_STRUCT_FIELD_TYPE");
            }

            // LLVM slot type: primitives + nested structs use the type's
            // own LLVM body inline; class refs use a single ptr slot so the
            // struct doesn't grow with the referent's body size and so the
            // class layout is decoupled from the struct's layout pass.
            if (isClassRef) {
                llvmMembers.push_back(
                    llvm::PointerType::get(*module->getLlvmContext(), 0));
            } else {
                llvmMembers.push_back(fieldType->getLlvmType());
            }
        }

        // Structs use compiler-chosen (natural) layout — packing is a view
        // concern (views match an external wire format). LLVM's default
        // struct body is unpacked, so passing `false` here is the default
        // alignment behavior.
        ((llvm::StructType*) llvmType)->setBody(
            llvm::ArrayRef<llvm::Type*>(llvmMembers), /*packed=*/false);

        // Register so other codegen sites can resolve the struct by canonical
        // name (mirrors CajetaView).
        CajetaModule::getStructureToModule()[canonical] = module;

        for (auto& methodEntry : methods) {
            methodEntry.second->generatePrototype();
        }

        // S9.2 — synthesize per-(struct, interface) vtable globals.
        // Done after method prototypes so each method's LLVM function
        // exists for fn-pointer harvesting. Resolves the implements
        // clause's qualified names to actual CajetaInterface instances
        // first (the inherited CajetaClass machinery — qImplemented was
        // populated by buildStructOrViewNode in S9.1).
        resolveImplementedInterfaces();
        synthesizeInterfaceVTables();
    }

    void CajetaStruct::synthesizeInterfaceVTables() {
        if (implementedInterfaces.empty()) return;

        auto& ctx = *module->getLlvmContext();
        auto* lmod = module->getLlvmModule();
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        std::string structCanonical = qName->toCanonical();

        // Helper to sanitize a name into something LLVM accepts as a
        // global symbol — mirrors the pattern in
        // CajetaClass::getOrCreateDropFunction.
        auto sanitize = [](std::string s) {
            for (char& c : s) {
                if (c == ':' || c == '.' || c == '<' || c == '>'
                        || c == ',' || c == ' ') {
                    c = '_';
                }
            }
            return s;
        };

        for (auto& iface : implementedInterfaces) {
            std::string ifaceCanonical = iface->getQName()->toCanonical();

            // Build the vtable entries in the interface's method
            // declaration order. Find each interface method's matching
            // concrete implementation on this struct by name. S9.3
            // tightens this with full signature matching.
            auto findConcrete = [&](MethodPtr ifaceMethod) -> MethodPtr {
                for (auto& [canon, m] : methods) {
                    if (!m || m->isConstructor()) continue;
                    if (m->getModifiers().find(STATIC) != m->getModifiers().end()) continue;
                    if (m->getName() == ifaceMethod->getName()) {
                        return m;
                    }
                }
                return nullptr;
            };

            std::vector<llvm::Constant*> entries;
            for (auto& ifaceMethod : iface->getMethodList()) {
                if (!ifaceMethod || ifaceMethod->isConstructor()) continue;
                if (ifaceMethod->getModifiers().find(STATIC)
                        != ifaceMethod->getModifiers().end()) continue;
                MethodPtr concrete = findConcrete(ifaceMethod);
                if (!concrete || !concrete->getLlvmFunction()) {
                    // Missing impl — S9.3 surfaces this with a clean
                    // CAJETA_ERROR_INTERFACE_METHOD_NOT_IMPLEMENTED.
                    // For now, fill the slot with null so the vtable
                    // global still emits (other entries may be usable).
                    entries.push_back(llvm::ConstantPointerNull::get(ptrTy));
                    continue;
                }
                entries.push_back(concrete->getLlvmFunction());
            }

            // Even an empty vtable still gets a global (zero-length
            // array). Through-interface dispatch sites consult the
            // global's address; the runtime indexes by method offset.
            llvm::ArrayType* arrTy = llvm::ArrayType::get(
                ptrTy, entries.size());
            std::string globalName = std::string("struct.")
                + sanitize(structCanonical) + "_iface_"
                + sanitize(ifaceCanonical) + "_VTable";
            if (llvm::GlobalVariable* existing = lmod->getNamedGlobal(globalName)) {
                interfaceVTables[ifaceCanonical] = existing;
                continue;
            }

            llvm::Constant* init = llvm::ConstantArray::get(arrTy, entries);
            auto* gv = new llvm::GlobalVariable(
                *lmod, arrTy, /*isConstant=*/true,
                llvm::GlobalValue::InternalLinkage, init, globalName);
            interfaceVTables[ifaceCanonical] = gv;
        }
    }

    uint64_t CajetaStruct::getFixedSize() const {
        if (!llvmType || !llvm::isa<llvm::StructType>(llvmType)) {
            return 0;
        }
        const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();
        return dl.getTypeAllocSize(llvmType);
    }

    llvm::Function* CajetaStruct::getOrCreateDropFunction() {
        if (llvmDropFunction) return llvmDropFunction;

        auto& ctx = *module->getLlvmContext();
        auto* lmod = module->getLlvmModule();
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::FunctionType* fnTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx), {(llvm::Type*) ptrTy},
            /*isVarArg=*/false);

        // Same name-sanitization pass as CajetaClass::getOrCreateDropFunction
        // so symbols read cleanly in stack traces.
        std::string dropName = std::string("__cajeta_struct_")
            + qName->toCanonical() + "_drop";
        for (char& c : dropName) {
            if (c == ':' || c == '.' || c == '<' || c == '>' || c == ',' || c == ' ') {
                c = '_';
            }
        }
        if (llvm::Function* existing = lmod->getFunction(dropName)) {
            llvmDropFunction = existing;
            return existing;
        }

        llvmDropFunction = llvm::Function::Create(fnTy,
            llvm::Function::ExternalLinkage, dropName, lmod);
        llvm::BasicBlock* bb = llvm::BasicBlock::Create(
            ctx, "entry", llvmDropFunction);
        llvm::IRBuilder<> b(bb);
        llvm::Value* instance = llvmDropFunction->getArg(0);

        // Null guard. The drop chain shouldn't fire on a null body alloca
        // in practice, but a deactivated entry whose owner pointer was
        // never populated reaches here as null — bail rather than walk
        // into garbage.
        llvm::BasicBlock* doDrop = llvm::BasicBlock::Create(
            ctx, "doDrop", llvmDropFunction);
        llvm::BasicBlock* done = llvm::BasicBlock::Create(
            ctx, "done", llvmDropFunction);
        llvm::Value* isNull = b.CreateICmpEQ(instance,
            llvm::ConstantPointerNull::get(ptrTy));
        b.CreateCondBr(isNull, done, doDrop);

        b.SetInsertPoint(doDrop);

        // Walk properties in REVERSE declaration order. Per Structs.md
        // § Drop chain: "owned class fields drop recursively in reverse
        // declaration order." For each class-ref field, GEP the slot,
        // load the pointer (the field slot is `ptr`-sized per S6.3),
        // call the class's own drop fn if non-null.
        //
        // v1 simplification: every class-ref field is treated as owned.
        // The aggregate initializer's per-binding ownership-transfer
        // ensures the source local's drop entry is deactivated when its
        // class instance flows into a struct field, so we don't double
        // free. Explicit borrow form (struct field holding a borrowed
        // reference whose drop the struct must skip) is deferred — it
        // requires per-instance ownership tracking the current chain
        // shape doesn't carry.
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        std::vector<StructurePropertyPtr> reversed(
            propertyList.begin(), propertyList.end());
        std::reverse(reversed.begin(), reversed.end());
        for (auto& property : reversed) {
            auto fieldType = property->getType();
            auto fieldClass = dynamic_pointer_cast<CajetaClass>(fieldType);
            if (!fieldClass) continue;
            if (dynamic_pointer_cast<CajetaAggregate>(fieldType)) continue;
            if (dynamic_pointer_cast<CajetaArray>(fieldType)) continue;
            if (fieldClass->isInterface()) continue;

            unsigned fieldIdx = (unsigned) getFieldLlvmIndex(property);
            llvm::Value* slotPtr = b.CreateStructGEP(
                llvmType, instance, fieldIdx,
                std::string("drop_field_") + property->getName());
            llvm::Value* refPtr = b.CreateLoad(ptrTy, slotPtr);

            // Call into the referent class's drop fn. Pull it through
            // ensureFunctionInModule so a cross-module reference (stdlib
            // class dropped from user code) gets a local extern decl that
            // the linker resolves at merge time.
            llvm::Function* refDrop = fieldClass->getOrCreateDropFunction();
            if (!refDrop) continue;
            refDrop = CajetaModule::ensureFunctionInModule(lmod, refDrop);
            b.CreateCall(refDrop, {refPtr});
        }

        // No __cajeta_free here — the struct body is stack-resident; the
        // function's epilogue reclaims it.

        b.CreateBr(done);
        b.SetInsertPoint(done);
        b.CreateRetVoid();
        return llvmDropFunction;
    }

} // namespace cajeta
