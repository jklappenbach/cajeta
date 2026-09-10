// CajetaView — see the header for the design. Owns the byte-overlay codegen
// (variable size, endianness, alignment) specific to views.

#include "CajetaView.h"
#include "CajetaArray.h"
#include "../compile/CajetaModule.h"
#include "../compile/CompilationContext.h"
#include "../method/Method.h"
#include "../error/Exception.h"

namespace cajeta {

    bool CajetaView::isVariableSize(const StructurePropertyPtr& property) {
        if (!property || !property->getType()) return false;
        auto type = property->getType();
        auto qn = type->getQName();
        if (qn && qn->getTypeName() == "String") return true;
        if (dynamic_pointer_cast<CajetaArray>(type)) return true;
        return false;
    }

    bool CajetaView::isElementArray(const StructurePropertyPtr& property) {
        if (!property || !property->getType()) return false;
        auto arr = dynamic_pointer_cast<CajetaArray>(property->getType());
        if (!arr) return false;
        auto elem = arr->getElementType();
        if (!elem) return false;
        if (dynamic_pointer_cast<CajetaView>(elem)) return true;
        auto qn = elem->getQName();
        return qn && qn->getTypeName() == "String";
    }

    bool CajetaView::needsBswap(CajetaModulePtr module, ViewEndianness e) {
        if (e == ViewEndianness::Host) return false;
        // Assumes a little-endian host (x86_64, aarch64), the same stance
        // DotExpression::maybeBswap takes.
        (void) module;
        const bool hostLittle = true;
        return (e == ViewEndianness::Big && hostLittle)
            || (e == ViewEndianness::Little && !hostLittle);
    }

    llvm::Value* CajetaView::emitSwapIfNeeded(CajetaModulePtr module,
            ViewEndianness e, llvm::Value* v) {
        if (!CajetaView::needsBswap(module, e)) return v;
        llvm::Type* t = v->getType();
        if (!t->isIntegerTy() || t->getIntegerBitWidth() <= 8) return v;
        llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
            module->getLlvmModule(), llvm::Intrinsic::bswap, {t});
        return module->getBuilder()->CreateCall(fn, {v});
    }

    static llvm::Value* swapIfNeeded(CajetaModulePtr module,
            ViewEndianness e, llvm::Value* v) {
        return CajetaView::emitSwapIfNeeded(module, e, v);
    }

    bool CajetaView::elementArrayHasVarSizeElements(
            const StructurePropertyPtr& property) {
        if (!isElementArray(property)) return false;
        auto arr = dynamic_pointer_cast<CajetaArray>(property->getType());
        auto elemView = dynamic_pointer_cast<CajetaView>(arr->getElementType());
        if (!elemView) return true;   // String[] — always var-size elements
        return elemView->getVariableSizeFieldCount() > 0;
    }

    int CajetaView::tableSlotOf(const StructurePropertyPtr& property) const {
        int slot = 0;
        bool sawVar = false;
        for (auto& p : propertyList) {
            bool v = isVariableSize(p);
            if (!sawVar && !v) continue;      // compile-time-constant offset
            if (v) sawVar = true;
            if (p == property) return slot;
            slot += elementArrayHasVarSizeElements(p) ? 2 : 1;
        }
        return -1;
    }

    int CajetaView::tableFixedSlotCount() const {
        int slot = 0;
        bool sawVar = false;
        for (auto& p : propertyList) {
            bool v = isVariableSize(p);
            if (!sawVar && !v) continue;
            if (v) sawVar = true;
            slot += elementArrayHasVarSizeElements(p) ? 2 : 1;
        }
        return slot;
    }

    llvm::Value* CajetaView::emitElementAdvance(CajetaModulePtr module,
            const shared_ptr<CajetaView>& elemView,
            llvm::Value* basePtr, llvm::Value* offset) {
        auto* builder = module->getBuilder();
        auto& ctx = *module->getLlvmContext();
        const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
        ViewEndianness elemE = elemView->getEndianness();
        for (auto& p : elemView->getPropertyList()) {
            if (CajetaView::isVariableSize(p)) {
                llvm::Value* pPtr = builder->CreateInBoundsGEP(
                    i8Ty, basePtr, offset, "elem_vlen_ptr");
                llvm::Value* len = builder->CreateIntCast(
                    swapIfNeeded(module, elemE,
                        builder->CreateLoad(i32Ty, pPtr)),
                    i64Ty, /*isSigned=*/true);
                uint64_t elemBytes = 1;
                if (auto arrType = dynamic_pointer_cast<CajetaArray>(p->getType())) {
                    if (auto et = arrType->getElementLlvmType(&ctx)) {
                        elemBytes = dl.getTypeAllocSize(et);
                        if (elemBytes == 0) elemBytes = 1;
                    }
                }
                llvm::Value* dataBytes = (elemBytes == 1) ? len
                    : builder->CreateMul(len,
                        llvm::ConstantInt::get(i64Ty, elemBytes));
                offset = builder->CreateAdd(
                    builder->CreateAdd(offset,
                        llvm::ConstantInt::get(i64Ty, 4)),
                    dataBytes, "elem_after_var");
            } else {
                uint64_t sz = dl.getTypeAllocSize(p->getType()->getLlvmType());
                offset = builder->CreateAdd(offset,
                    llvm::ConstantInt::get(i64Ty, sz), "elem_after_fixed");
            }
        }
        return offset;
    }

    llvm::Value* CajetaView::emitAccessAdvance(CajetaModulePtr module,
            const StructurePropertyPtr& property,
            llvm::Value* basePtr, llvm::Value* offset,
            ViewEndianness e) {
        auto* builder = module->getBuilder();
        auto& ctx = *module->getLlvmContext();
        const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);

        if (!CajetaView::isVariableSize(property)) {
            uint64_t sz = dl.getTypeAllocSize(property->getType()->getLlvmType());
            return builder->CreateAdd(offset,
                llvm::ConstantInt::get(i64Ty, sz), "adv_fixed");
        }

        if (CajetaView::isElementArray(property)) {
            llvm::Value* cPtr = builder->CreateInBoundsGEP(
                i8Ty, basePtr, offset, "adv_count_ptr");
            llvm::Value* count = builder->CreateIntCast(
                swapIfNeeded(module, e, builder->CreateLoad(i32Ty, cPtr)),
                i64Ty, /*isSigned=*/true);
            llvm::Value* start = builder->CreateAdd(offset,
                llvm::ConstantInt::get(i64Ty, 4), "adv_elems_start");
            shared_ptr<CajetaView> elemView;
            if (auto arrType = dynamic_pointer_cast<CajetaArray>(
                    property->getType())) {
                elemView = dynamic_pointer_cast<CajetaView>(
                    arrType->getElementType());
            }
            if (elemView && elemView->getVariableSizeFieldCount() == 0) {
                uint64_t stride = elemView->getFixedSize();
                return builder->CreateAdd(start,
                    builder->CreateMul(count,
                        llvm::ConstantInt::get(i64Ty, stride)),
                    "adv_stride_end");
            }
            llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
            llvm::BasicBlock* preBB = builder->GetInsertBlock();
            llvm::BasicBlock* hdrBB = llvm::BasicBlock::Create(
                ctx, "adv_earr_hdr", parentFn);
            llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(
                ctx, "adv_earr_body", parentFn);
            llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(
                ctx, "adv_earr_exit", parentFn);
            builder->CreateBr(hdrBB);
            builder->SetInsertPoint(hdrBB);
            llvm::PHINode* kPhi = builder->CreatePHI(i64Ty, 2, "adv_k");
            llvm::PHINode* offPhi = builder->CreatePHI(i64Ty, 2, "adv_off");
            kPhi->addIncoming(llvm::ConstantInt::get(i64Ty, 0), preBB);
            offPhi->addIncoming(start, preBB);
            builder->CreateCondBr(
                builder->CreateICmpSLT(kPhi, count), bodyBB, exitBB);
            builder->SetInsertPoint(bodyBB);
            llvm::Value* offAfter;
            if (elemView) {
                offAfter = emitElementAdvance(module, elemView, basePtr, offPhi);
            } else {
                llvm::Value* pPtr = builder->CreateInBoundsGEP(
                    i8Ty, basePtr, offPhi, "adv_slen_ptr");
                llvm::Value* len = builder->CreateIntCast(
                    swapIfNeeded(module, e, builder->CreateLoad(i32Ty, pPtr)),
                    i64Ty, /*isSigned=*/true);
                offAfter = builder->CreateAdd(
                    builder->CreateAdd(offPhi,
                        llvm::ConstantInt::get(i64Ty, 4)),
                    len, "adv_after_str");
            }
            llvm::Value* kNext = builder->CreateAdd(kPhi,
                llvm::ConstantInt::get(i64Ty, 1));
            llvm::BasicBlock* bodyEndBB = builder->GetInsertBlock();
            builder->CreateBr(hdrBB);
            kPhi->addIncoming(kNext, bodyEndBB);
            offPhi->addIncoming(offAfter, bodyEndBB);
            builder->SetInsertPoint(exitBB);
            return offPhi;
        }

        llvm::Value* pPtr = builder->CreateInBoundsGEP(
            i8Ty, basePtr, offset, "adv_vlen_ptr");
        llvm::Value* len = builder->CreateIntCast(
            swapIfNeeded(module, e, builder->CreateLoad(i32Ty, pPtr)),
            i64Ty, /*isSigned=*/true);
        uint64_t elemBytes = 1;
        if (auto arrType = dynamic_pointer_cast<CajetaArray>(property->getType())) {
            if (auto et = arrType->getElementLlvmType(&ctx)) {
                elemBytes = dl.getTypeAllocSize(et);
                if (elemBytes == 0) elemBytes = 1;
            }
        }
        llvm::Value* dataBytes = (elemBytes == 1) ? len
            : builder->CreateMul(len, llvm::ConstantInt::get(i64Ty, elemBytes));
        return builder->CreateAdd(
            builder->CreateAdd(offset, llvm::ConstantInt::get(i64Ty, 4)),
            dataBytes, "adv_after_var");
    }

    llvm::Type* CajetaView::getLlvmType() {
        if (isFrozen() && CajetaType::rawLlvmType() == nullptr) {
            llvm::LLVMContext* ctx = currentLlvmContext();
            if (!ctx && module) ctx = module->getLlvmContext();
            if (ctx) {
                // Members are exactly the fixed fields before the first
                // variable-size one, as in generatePrototype.
                std::string canonical = qName->toCanonical();
                llvm::StructType* st = CajetaType::getOrCreateLlvmStructNoRegister(ctx, canonical);
                setLlvmType(st);  // U6.4.2: bind before setBody so refs resolve
                std::vector<llvm::Type*> members;
                members.reserve(propertyList.size());
                bool sawVariableSize = false;
                for (auto& property : propertyList) {
                    if (CajetaView::isVariableSize(property)) { sawVariableSize = true; continue; }
                    if (sawVariableSize) continue;
                    members.push_back(property->getType()->getLlvmType());
                }
                const bool packed = (alignment != ViewAlignment::Natural);
                st->setBody(llvm::ArrayRef<llvm::Type*>(members), packed);
            }
        }
        return CajetaClass::getLlvmType();
    }

    void CajetaView::generatePrototype() {
        string canonical = qName->toCanonical();

        // An unannotated view defaults to host order, and `endiannessExplicit`
        // deliberately stays false so a nested view can still inherit its
        // outer's order instead of being pinned to host.

        // getOrCreateLlvmType also puts a plain CajetaType in the canonical map;
        // the two lines below overwrite it with this CajetaView.
        setLlvmType(CajetaType::getOrCreateLlvmType(module->getLlvmContext(), canonical));  // U6.2
        typeMap[TypeKey(rawLlvmType())] = shared_from_this();
        canonicalMap[canonical] = static_pointer_cast<CajetaType>(shared_from_this());
        // Short name too: the `MyView(byte[])` constructor lookup is unqualified.
        canonicalMap[qName->getTypeName()] = static_pointer_cast<CajetaType>(shared_from_this());
        typeFlags = STRUCT_FLAG | USER_DEFINED_FLAG;

        // Layout: the LLVM struct holds only the fixed-size fields BEFORE the
        // first variable-size one. Every `String`/`T[]` payload (i32 length +
        // data) and every post-var fixed field lives past the struct footprint.
        vector<llvm::Type*> llvmMembers;
        llvmMembers.reserve(propertyList.size());
        bool sawVariableSize = false;
        int variableSizeCount = 0;
        for (auto& property : propertyList) {
            // Direct self-containment only; transitive cycles are not caught.
            auto fieldType = property->getType();
            if (fieldType && fieldType->getQName()
                    && fieldType->getQName()->toCanonical() == canonical) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                    "view '%s' has field '%s' of its own type; recursive views are "
                    "forbidden (would have infinite size). Use a class reference if "
                    "you need recursive structure.",
                    canonical.c_str(), property->getName().c_str());
                throw Exception(buf, "CAJETA_ERROR_VIEW_RECURSIVE");
            }
            if (auto arr = dynamic_pointer_cast<CajetaArray>(fieldType)) {
                if (dynamic_pointer_cast<CajetaArray>(arr->getElementType())) {
                    char buf[256];
                    snprintf(buf, sizeof(buf),
                        "view '%s' field '%s' is an array of arrays; element "
                        "arrays in views are single-level (see "
                        "specs/view-element-arrays-spec.md). Flatten the "
                        "layout or wrap the inner array in its own view type.",
                        canonical.c_str(), property->getName().c_str());
                    throw Exception(buf, "CAJETA_ERROR_VIEW_NESTED_ELEMENT_ARRAY");
                }
                if (auto elemView = dynamic_pointer_cast<CajetaView>(
                        arr->getElementType())) {
                    if (!elemView->hasExplicitEndianness()
                            && endianness != ViewEndianness::Host) {
                        if (elemView->hasInheritedEndianness()
                                && elemView->getEndianness() != endianness) {
                            char buf[320];
                            snprintf(buf, sizeof(buf),
                                "element view '%s' inherits conflicting "
                                "endianness from multiple outer views "
                                "(via '%s' field '%s'); annotate the element "
                                "view explicitly (@BigEndian / @LittleEndian "
                                "/ @HostEndian).",
                                elemView->getQName()->getTypeName().c_str(),
                                canonical.c_str(),
                                property->getName().c_str());
                            throw Exception(buf,
                                "CAJETA_ERROR_VIEW_ENDIAN_AMBIGUOUS");
                        }
                        if (!elemView->hasInheritedEndianness()) {
                            elemView->inheritEndianness(endianness);
                        }
                    }
                    for (auto& ep : elemView->getPropertyList()) {
                        if (CajetaView::isElementArray(ep)) {
                            char buf[320];
                            snprintf(buf, sizeof(buf),
                                "view '%s' field '%s': element view '%s' "
                                "itself declares element-array field '%s'; "
                                "element arrays in views are single-level "
                                "(see specs/view-element-arrays-spec.md).",
                                canonical.c_str(), property->getName().c_str(),
                                elemView->getQName()->getTypeName().c_str(),
                                ep->getName().c_str());
                            throw Exception(buf,
                                "CAJETA_ERROR_VIEW_NESTED_ELEMENT_ARRAY");
                        }
                    }
                }
            }
            bool isVar = CajetaView::isVariableSize(property);
            if (CajetaView::isElementArray(property)) {
                hasElementArrayField_ = true;
            }
            if (isVar) {
                sawVariableSize = true;
                variableSizeCount += 1;
            } else if (sawVariableSize) {
                // Deliberately no member: a fixed field after a variable-size
                // one has no constant offset, so its accessor walks at runtime.
            } else {
                llvmMembers.push_back(property->getType()->getLlvmType());
            }
        }
        variableSizeFieldCount = variableSizeCount;
        const bool packed = (alignment != ViewAlignment::Natural);
        ((llvm::StructType*) rawLlvmType())->setBody(
            llvm::ArrayRef<llvm::Type*>(llvmMembers), packed);

        CajetaModule::getStructureToModule()[canonical] = module;

        for (auto& methodEntry : methods) {
            methodEntry.second->generatePrototype();
        }
    }

    uint64_t CajetaView::getFixedSize() const {
        llvm::Type* lt = rawLlvmType();
        if (!lt || !llvm::isa<llvm::StructType>(lt)) {
            return 0;
        }
        const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();
        return dl.getTypeAllocSize(lt);
    }

    uint64_t CajetaView::getMinimumSize() const {
        return getFixedSize() + (uint64_t) variableSizeFieldCount * 4;
    }

} // namespace cajeta
