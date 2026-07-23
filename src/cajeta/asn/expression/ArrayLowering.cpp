#include "ArrayLowering.h"

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>

#include "../AbstractSyntaxNode.h"
#include "../../compile/CajetaModule.h"
#include "../../type/CajetaArray.h"

namespace cajeta {

    namespace {
        // Coerce a produced value to the element slot's width. Preserves the
        // prior int->int behavior of the `{...}` path and adds int->float and
        // float->float so a unified/target float element type stores cleanly.
        // Pointers and reference elements store directly (no coercion).
        // NOTE: unsigned integer WIDENING is sign-extended upstream (a general
        // compiler-wide coercion bug — `int64 y = someUint32` sexts too, not
        // just arrays), so the source arrives here already widened; fixing that
        // belongs in the shared numeric-coercion path, not this helper.
        llvm::Value* coerceToElement(llvm::IRBuilder<>* b, llvm::Value* v,
                                     llvm::Type* elemTy) {
            llvm::Type* vt = v->getType();
            if (vt == elemTy) return v;
            if (elemTy->isIntegerTy() && vt->isIntegerTy())
                return b->CreateIntCast(v, elemTy, /*isSigned=*/true);
            if (elemTy->isFloatingPointTy() && vt->isIntegerTy())
                return b->CreateSIToFP(v, elemTy);
            if (elemTy->isFloatingPointTy() && vt->isFloatingPointTy())
                return b->CreateFPCast(v, elemTy);
            return v;
        }
    } // namespace

    llvm::Value* emitArrayFromElements(
            CajetaModulePtr module,
            CajetaTypePtr elementType,
            const std::vector<AbstractSyntaxNodePtr>& elements,
            bool useArena) {
        if (!elementType) return nullptr;

        auto* builder = module->getBuilder();
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();

        // Build the array type once so we can size header + element slots, and
        // register it (structures) as the declarator path does.
        auto arrayType = std::make_shared<CajetaArray>(module, elementType);
        module->getStructures()[arrayType->toCanonical()] =
            std::static_pointer_cast<CajetaClass>(arrayType);

        // Arena (stack) placement is primitive-element only, so it never needs
        // the droppable-bits allocator; heap picks bits when the element carries
        // per-slot ownership. Same 3-arg signature for all three.
        const char* allocSym = useArena
            ? "__cajeta_new_array_header_arena"
            : (CajetaClass::arrayElementCarriesSlotBits(elementType)
                ? "__cajeta_new_array_header_bits"
                : "__cajeta_new_array_header");
        llvm::Function* allocFn = module->getRuntimeFunction(allocSym);
        if (!allocFn) return nullptr;

        llvm::Type* headerTy = arrayType->getLlvmType();
        llvm::Type* elemTy = arrayType->getElementLlvmType(&ctx);
        uint64_t headerBytes = dl.getTypeAllocSize(headerTy);
        uint64_t elemBytes = arrayType->elementStrideBytes(dl, &ctx);

        int64_t count = (int64_t) elements.size();
        llvm::Value* hdrPtr = builder->CreateCall(allocFn, {
            llvm::ConstantInt::get(i64Ty, headerBytes),
            llvm::ConstantInt::get(i64Ty, elemBytes),
            llvm::ConstantInt::get(i64Ty, count),
        });

        // Write each element into its data slot. GEP path:
        // pointer -> struct -> data array -> element[idx]. The layout the
        // runtime helpers and ArrayCreatorRest use.
        int idx = 0;
        for (auto& node : elements) {
            llvm::Value* v = node->generateCode(module);
            if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(v)) {
                v = builder->CreateLoad(a->getAllocatedType(), a);
            }
            if (!v) { ++idx; continue; }
            v = coerceToElement(builder, v, elemTy);
            std::vector<llvm::Value*> gepIndices = {
                llvm::ConstantInt::get(i64Ty, 0),
                llvm::ConstantInt::get(i32Ty, CajetaArray::DATA_FIELD_INDEX),
                llvm::ConstantInt::get(i64Ty, idx),
            };
            llvm::Value* slot = builder->CreateGEP(headerTy, hdrPtr, gepIndices);
            builder->CreateStore(v, slot);
            ++idx;
        }
        return hdrPtr;
    }

} // namespace cajeta
