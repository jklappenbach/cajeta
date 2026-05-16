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
            // is exercised by real code.
            if (fieldType && fieldType->getQName()
                    && fieldType->getQName()->toCanonical() == canonical) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                    "struct '%s' has field '%s' of its own type; recursive structs "
                    "are forbidden (would have infinite size). Use a class reference "
                    "if you need recursive structure (lands in S6.3).",
                    canonical.c_str(), property->getName().c_str());
                throw Exception(buf, "CAJETA_ERROR_STRUCT_RECURSIVE");
            }

            // Reject T[] and String — both are variable-tail/heap-backed
            // and have no place in a fixed-size stack aggregate. Views
            // accept these via length-prefix encoding; structs don't.
            // S6.6 generalizes this rejection.
            if (dynamic_pointer_cast<CajetaArray>(fieldType)) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                    "struct '%s' field '%s' has array type — arrays are heap-backed "
                    "and don't fit in a fixed-size stack aggregate. Put the array in "
                    "an enclosing class instead.",
                    canonical.c_str(), property->getName().c_str());
                throw Exception(buf, "CAJETA_ERROR_STRUCT_FIELD_TYPE");
            }
            // Views in struct fields are deferred — viable in principle
            // (a view is itself an aggregate) but unexercised; reject for
            // now with a clear pointer.
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
    }

    uint64_t CajetaStruct::getFixedSize() const {
        if (!llvmType || !llvm::isa<llvm::StructType>(llvmType)) {
            return 0;
        }
        const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();
        return dl.getTypeAllocSize(llvmType);
    }

} // namespace cajeta
