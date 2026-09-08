#include "ArrayLowering.h"

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>

#include "../AbstractSyntaxNode.h"
#include "../../compile/CajetaModule.h"
#include "../../ownership/TitleClassifier.h"
#include "../../type/CajetaArray.h"
#include "../../type/CajetaClass.h"
#include "../../type/CajetaView.h"
#include "Expression.h"

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
            bool useArena,
            std::vector<std::pair<int, std::string>>* borrowedLocals) {
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
            : ((CajetaClass::arrayElementCarriesSlotBits(elementType)
                    || CajetaClass::arrayElementCarriesArraySlotBits(elementType))
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

        // ownership-title-classifier spec 5.10 — an element is a STORE into
        // the fresh array's slot and follows the store rule, exactly as
        // `a[i] = e` does: a bare name lends (a String slot resolves its own
        // copy — resident String slots always own), `#x` transfers, a fresh
        // value is the slot's, a literal is a borrow of static storage, a
        // call rides its flag. One classifier answer per element decides
        // the title the slot records; before this the literal stored raw
        // pointers, so `[heap Cell(1)]` leaked its cell (5.6) and `[.., #out]`
        // adopted a wrapper into an unmarked slot and leaked it (5.10's
        // witness). An arena literal (primitive elements only) keeps the raw
        // store.
        const bool elemIsString = [&] {
            auto ec = std::dynamic_pointer_cast<CajetaClass>(elementType);
            return ec && !std::dynamic_pointer_cast<CajetaView>(elementType)
                && ec->getQName() && ec->getQName()->getTypeName() == "String"
                && ec->getQName()->getPackageName() == "cajeta.lang";
        }();
        const bool elemTailBits = !useArena && !elemIsString
            && CajetaClass::arrayElementCarriesSlotBits(elementType);
        const bool elemArrBits = !useArena && !elemIsString && !elemTailBits
            && CajetaClass::arrayElementCarriesArraySlotBits(elementType);
        llvm::Function* strStoreFn = (elemIsString && !useArena)
            ? module->getRuntimeFunction("__cajeta_string_elem_store") : nullptr;
        llvm::Function* tailStoreFn = elemTailBits
            ? module->getRuntimeFunction("__cajeta_tail_elem_store") : nullptr;
        llvm::Function* arrStoreFn = elemArrBits
            ? module->getRuntimeFunction("__cajeta_tail_arrelem_store") : nullptr;

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
            auto elemExpr = std::dynamic_pointer_cast<Expression>(node);
            if (strStoreFn || tailStoreFn || arrStoreFn) {
                // The title the slot takes: 1 (owned), a runtime flag, or 0.
                llvm::Value* title = nullptr;
                if (elemExpr) {
                    ownership::TitleShape es = ownership::classify(elemExpr, module);
                    title = ownership::storeTitleFlagOf(
                        es, strStoreFn ? ownership::ConsumerRole::StoreString
                                       : ownership::ConsumerRole::StoreSlot,
                        elemExpr, module, "an array literal element");
                    // A bare frame local that OWNS its value lends it: the
                    // slot dies with the local, so remember which slot and
                    // the binding refuses the array an escape (spec 5.10).
                    // Not for a String slot (it resolved its own copy), and
                    // only for a local that PROVABLY owns (a static entry):
                    // an entry-less local holds someone else's value, and a
                    // runtime-flagged one (`Class<?> c = registryAt(i)`) is
                    // the borrow-collecting idiom — the callee's lend is the
                    // programmer's assertion, as it is for any plain return.
                    if (borrowedLocals && !strStoreFn
                            && es.family == ownership::TitleFamily::LocalRead
                            && es.field && es.field->getDropEntry()
                            && !es.field->isRuntimeConditionalOwner()
                            && !es.has(ownership::TitleShape::kIsParam)) {
                        borrowedLocals->emplace_back(idx, es.field->getName());
                    }
                }
                if (!title) title = llvm::ConstantInt::get(i64Ty, 0);
                if (strStoreFn) {
                    builder->CreateCall(strStoreFn, {slot, v, title});
                } else if (tailStoreFn) {
                    builder->CreateCall(tailStoreFn, {
                        hdrPtr, llvm::ConstantInt::get(i64Ty, headerBytes),
                        llvm::ConstantInt::get(i64Ty, elemBytes),
                        llvm::ConstantInt::get(i64Ty, (uint64_t) idx), v, title});
                } else {
                    builder->CreateCall(arrStoreFn, {
                        hdrPtr, llvm::ConstantInt::get(i64Ty, headerBytes),
                        llvm::ConstantInt::get(i64Ty, elemBytes),
                        llvm::ConstantInt::get(i64Ty, (uint64_t) idx), v, title,
                        llvm::ConstantInt::get(i64Ty,
                            CajetaClass::arrayElementInnerDropKind(elementType))});
                }
            } else {
                builder->CreateStore(v, slot);
            }
            ++idx;
        }
        return hdrPtr;
    }

} // namespace cajeta
