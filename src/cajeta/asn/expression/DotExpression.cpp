//
// Created by James Klappenbach on 4/14/23.
//

#include "cajeta/xref/XrefIndex.h"
#include "../../error/Diagnostics.h"
#include "DotExpression.h"
#include "../../compile/CajetaModule.h"
#include "../../type/CajetaClass.h"
#include "../../type/CajetaView.h"
#include "../../type/CajetaView.h"
#include "../../type/CajetaArray.h"
#include "../../type/CajetaVector.h"
#include "../../type/VectorOps.h"
#include "../../error/Exception.h"
#include <functional>
#include "Identifier.h"

#include <climits>
#include <cmath>
#include <llvm/IR/Intrinsics.h>

namespace cajeta {
    DotExpression::DotExpression(CajetaParser::ExpressionContext* ctx, antlr4::Token* token) : Expression(token) { exprKind = ExprKind::Dot;
        // The DOT grammar allows several rhs forms; only the identifier form is
        // implemented — other forms capture an empty name so the lhs errors.
        if (ctx->identifier()) {
            identifier = ctx->identifier()->getText();
            if (auto* idTok = ctx->identifier()->getStart()) {
                idLine   = (int) idTok->getLine();
                idColumn = (int) idTok->getCharPositionInLine();
            }
        }
    }

    // Resolve the LHS, look this member up on its resolved class type, and pin
    // our own type to the member's — enum constants and statics included.
    void DotExpression::resolveTypes(CajetaModulePtr module) {
        AbstractSyntaxNode::resolveTypes(module);
        if (children.empty()) {
            return;
        }
        auto lhs = dynamic_pointer_cast<Expression>(children[0]);
        if (!lhs) {
            return;
        }
        // Enum constant `MyEnum.NAME`: bound here so diamond inference sees int32.
        if (auto id = dynamic_pointer_cast<IdentifierExpression>(lhs)) {
            const string& ns = id->getTextValue();
            if (CajetaType::lookupEnumConstant(ns, identifier).has_value()) {
                // int32, NOT the enum type, is deliberate: resolving to the enum
                // broke enum `==` across the stdlib. The gap closes in subtypeDistance.
                resolvedType = CajetaType::of("int32");
                return;
            }
            // Static field `Counter.total`: the LHS names a class, not a local, and
            // resolves through ofScoped — the raw short-name key is last-writer-wins.
            if (!lhs->getResolvedType()) {
                auto scoped = CajetaType::ofScoped(ns, module);
                if (auto staticKlass = dynamic_pointer_cast<CajetaClass>(scoped)) {
                    lhs->setResolvedType(staticKlass);
                }
            }
        }
        // Vector component access: v.x/.y/.z/.w (and .r/.g/.b/.a). An invalid
        // component is left unresolved here; generateCode emits the diagnostic.
        if (auto vecT = dynamic_pointer_cast<CajetaVector>(lhs->getResolvedType())) {
            int lane = vecops::laneForComponentName(identifier);
            if (lane >= 0 && (unsigned) lane < vecT->getLanes()) {
                resolvedType = vecT->getElementType();      // single component
            } else {
                auto lanes = vecops::swizzleLanes(identifier);
                bool ok = !lanes.empty();
                for (int l : lanes)
                    if ((unsigned) l >= vecT->getLanes()) ok = false;
                if (ok)
                    resolvedType = CajetaVector::getOrCreate(
                        module, vecT->getElementType(), (uint32_t) lanes.size());
            }
            return;
        }
        auto klass = dynamic_pointer_cast<CajetaClass>(lhs->getResolvedType());
        if (!klass) {
            return;
        }
        // Walk the inheritance chain — inherited fields live on ancestors'
        // properties maps, not on the subclass's own.
        std::function<bool(const CajetaClassPtr&)> findProp =
            [&](const CajetaClassPtr& cls) -> bool {
                auto pit = cls->getProperties().find(identifier);
                if (pit != cls->getProperties().end()) {
                    // Capture conversion: a bounded-wildcard receiver projects to its bound.
                    resolvedType = CajetaType::captureProject(
                        pit->second->getType());
                    // `cls`, not `klass`, is the class that DECLARES this field:
                    // for an inherited field the two differ.
                    recordFieldXref(cls);
                    return true;
                }
                for (auto& parent : cls->getSuperClasses()) {
                    if (findProp(parent)) return true;
                }
                return false;
            };
        findProp(klass);
    }

    // Record `receiver.field` as a reference to the field's declaration. No-op
    // unless --emit-xref, and skipped for synthesized source, which has no positions.
    void DotExpression::recordFieldXref(const CajetaClassPtr& owner) {
        if (!xref::captureEnabled() || !owner || idLine <= 0) return;
        const string& file = getSourceFile();
        if (file.empty()) return;

        // An instantiation has no source; its field is declared on the template.
        string ownerFqn = owner->getQName()->toCanonical();
        auto lt = ownerFqn.find('<');
        if (lt != string::npos) ownerFqn = ownerFqn.substr(0, lt);

        xref::noteFieldReference(ownerFqn + "." + identifier, file, idLine, idColumn);
    }

    llvm::Value* DotExpression::maybeBswap(CajetaModulePtr module, llvm::Value* v,
                                              const ExpressionPtr& receiver) {
        if (!v || !receiver) return v;
        auto recvType = receiver->getResolvedType();
        if (!recvType) return v;
        // Endianness is view-only — structs are host-endian. Cast to CajetaView so
        // a future struct receiver no-ops through here.
        auto viewType = dynamic_pointer_cast<CajetaView>(recvType);
        if (!viewType) return v;
        ViewEndianness e = viewType->getEndianness();
        if (e == ViewEndianness::Host) return v;
        // v1 assumption: the host is little-endian (x86_64, aarch64).
        const bool hostLittle = true;
        bool needBswap = (e == ViewEndianness::Big && hostLittle)
                      || (e == ViewEndianness::Little && !hostLittle);
        if (!needBswap) return v;
        llvm::Type* t = v->getType();
        if (!t->isIntegerTy()) return v;          // float bswap is post-v1
        if (t->getIntegerBitWidth() <= 8) return v;  // single byte has no byte order
        llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
            module->getLlvmModule(), llvm::Intrinsic::bswap, {t});
        return module->getBuilder()->CreateCall(fn, {v});
    }

    StructurePropertyPtr DotExpression::resolveViewElementArrayProperty(
            CajetaModulePtr module) {
        if (children.empty()) return nullptr;
        auto recv = dynamic_pointer_cast<Expression>(children[0]);
        if (!recv) return nullptr;
        if (!recv->getResolvedType()) recv->resolveTypes(module);
        auto viewType = dynamic_pointer_cast<CajetaView>(recv->getResolvedType());
        if (!viewType) return nullptr;
        for (auto& p : viewType->getPropertyList()) {
            if (p->getName() == identifier) {
                if (!CajetaView::isElementArray(p)) return nullptr;
                earrViewType = viewType;
                return p;
            }
        }
        return nullptr;
    }

    string DotExpression::buildPath(const ExpressionPtr& expr) {
        if (auto id = dynamic_pointer_cast<IdentifierExpression>(expr)) {
            return id->getTextValue();
        }
        if (auto dot = dynamic_pointer_cast<DotExpression>(expr)) {
            const auto& children = const_cast<DotExpression*>(dot.get())->getChildren();
            if (children.empty()) return "";
            auto lhs = dynamic_pointer_cast<Expression>(children[0]);
            if (!lhs) return "";
            string lhsPath = buildPath(lhs);
            if (lhsPath.empty()) return "";
            return lhsPath + "." + dot->getIdentifier();
        }
        return "";
    }

    // `a.b` lowers to a struct GEP. The lhs may be the alloca holding the struct or
    // a pointer loaded from the heap; both yield an address to GEP into, and the
    // member index comes from StructureProperty::getOrder(), set at signature pass.
    llvm::Value* DotExpression::generateCode(CajetaModulePtr module) {
        if (children.empty()) {
            return nullptr;
        }

        // A transferred PATH is readable, exactly as a transferred identifier is:
        // `#person.name` demotes to a borrow rather than killing the path.

        // Variable obscures type (JLS 6.4.2): an identifier naming both an in-scope
        // local and a class means the LOCAL. Four short-circuits below consume it.
        FieldPtr obscuringLocal;
        if (auto idLhs = dynamic_pointer_cast<IdentifierExpression>(children[0])) {
            if (auto sc = module->getScopeStack().peek()) {
                obscuringLocal = sc->getField(idLhs->getTextValue());
            }
        }

        // Static-namespace constants (Math.PI, Integer.MAX_VALUE) have no instance
        // backing and do not survive the GEP path, so they emit as IR constants here.
        if (auto idExpr = obscuringLocal
                ? nullptr
                : dynamic_pointer_cast<IdentifierExpression>(children[0])) {
            auto& ctx = *module->getLlvmContext();
            const std::string& ns = idExpr->getTextValue();
            if (ns == "Math") {
                if (identifier == "PI") return llvm::ConstantFP::get(
                    llvm::Type::getDoubleTy(ctx), M_PI);
                if (identifier == "E")  return llvm::ConstantFP::get(
                    llvm::Type::getDoubleTy(ctx), M_E);
            } else if (ns == "Integer") {
                if (identifier == "MAX_VALUE") return llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(ctx), INT32_MAX, /*isSigned=*/true);
                if (identifier == "MIN_VALUE") return llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(ctx), INT32_MIN, /*isSigned=*/true);
            } else if (ns == "Long") {
                if (identifier == "MAX_VALUE") return llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(ctx), INT64_MAX, /*isSigned=*/true);
                if (identifier == "MIN_VALUE") return llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(ctx), INT64_MIN, /*isSigned=*/true);
            }
            // Enum constant: the ordinal i32; the constant table is a side-map.
            if (auto v = CajetaType::lookupEnumConstant(ns, identifier)) {
                // Typed as the ENUM, not raw int32, so `Verb.POST.weight()` reaches
                // the companion class. The VALUE is unchanged — still the ordinal.
                auto& cmap = CajetaType::getCanonicalMap();
                auto et = cmap.find(ns);
                if (et != cmap.end()) {
                    resolvedType = et->second;
                }
                return llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(ctx), *v, /*isSigned=*/true);
            }
        }

        // `Counter.total`: IdentifierExpression returns null for a class name, so the
        // GEP path would bail; route to the static global, as a writable l-value.
        if (auto idLhs = dynamic_pointer_cast<IdentifierExpression>(children[0])) {
            // ofScoped, not the raw short-name key (see resolveTypes above).
            auto scoped = obscuringLocal
                ? nullptr
                : CajetaType::ofScoped(idLhs->getTextValue(), module);
            {
                if (auto staticKlass = dynamic_pointer_cast<CajetaClass>(scoped)) {
                    // A static declared on a base class is visible through derived names.
                    StructurePropertyPtr staticProp;
                    std::function<bool(const CajetaClassPtr&)> findStatic =
                        [&](const CajetaClassPtr& cls) -> bool {
                            auto pit = cls->getProperties().find(identifier);
                            if (pit != cls->getProperties().end()
                                    && pit->second->isStatic()) {
                                staticProp = pit->second;
                                return true;
                            }
                            for (auto& parent : cls->getSuperClasses()) {
                                if (findStatic(parent)) return true;
                            }
                            return false;
                        };
                    if (findStatic(staticKlass)) {
                        resolvedType = CajetaType::captureProject(
                            staticProp->getType());
                        return staticKlass->getOrCreateStaticFieldGlobal(
                            staticProp, module);
                    }
                }
            }
        }

        llvm::Value* base = children[0]->generateCode(module);
        if (!base) {
            return nullptr;
        }

        auto lhs = dynamic_pointer_cast<Expression>(children[0]);
        if (!lhs) {
            return nullptr;
        }
        // Variable obscures type, part 2: when the receiver IS an in-scope local its
        // declared type wins over whatever the pre-pass pinned.
        if (obscuringLocal && obscuringLocal->getType()
            && obscuringLocal->getType() != lhs->getResolvedType()) {
            lhs->setResolvedType(obscuringLocal->getType());
        }
        // Re-run resolveTypes when the lhs went unresolved in the pre-pass: locals
        // enter scope only when their declarations run at codegen time.
        if (!lhs->getResolvedType()) {
            lhs->resolveTypes(module);
        }
        // Vector component read: v.x/.y/.z/.w (and .r/.g/.b/.a) -> extractelement.
        if (auto vecT = dynamic_pointer_cast<CajetaVector>(lhs->getResolvedType())) {
            int lane = vecops::laneForComponentName(identifier);
            if (lane >= 0) {
                if ((unsigned) lane >= vecT->getLanes()) {
                    throw Exception(
                        "component '." + identifier + "' is out of range for "
                        "Vector<...," + std::to_string(vecT->getLanes()) + ">",
                        "CAJETA_ERROR_VECTOR_COMPONENT");
                }
                llvm::Value* vecVal = loadIfLValue(module, base, lhs);
                resolvedType = vecT->getElementType();
                return vecops::extractLane(*module->getBuilder(), vecVal,
                                           (unsigned) lane);
            }
            auto lanes = vecops::swizzleLanes(identifier);
            if (lanes.empty()) {
                throw Exception(
                    "'" + identifier + "' is not a vector component or swizzle "
                    "(use .x/.y/.z/.w, .r/.g/.b/.a, or a 2-4 letter swizzle "
                    "like .xyz)", "CAJETA_ERROR_VECTOR_COMPONENT");
            }
            for (int l : lanes) {
                if ((unsigned) l >= vecT->getLanes()) {
                    throw Exception(
                        "swizzle '." + identifier + "' references a lane out of "
                        "range for Vector<...," + std::to_string(vecT->getLanes())
                        + ">", "CAJETA_ERROR_VECTOR_COMPONENT");
                }
            }
            llvm::Value* vecVal = loadIfLValue(module, base, lhs);
            resolvedType = CajetaVector::getOrCreate(
                module, vecT->getElementType(), (uint32_t) lanes.size());
            return vecops::swizzle(*module->getBuilder(), vecVal, lanes);
        }
        auto klass = dynamic_pointer_cast<CajetaClass>(lhs->getResolvedType());
        if (!klass) {
            return nullptr;
        }
        // Gather over the FULL parent set: inherited fields live on the parent's map,
        // and a name found on two unrelated siblings is ambiguous unless shadowed.
        StructurePropertyPtr lookedUpProperty;
        std::vector<std::pair<CajetaClassPtr, StructurePropertyPtr>> allMatches;
        std::function<void(const CajetaClassPtr&)> gatherProps =
            [&](const CajetaClassPtr& cls) {
                auto pit = cls->getProperties().find(identifier);
                if (pit != cls->getProperties().end()) {
                    allMatches.push_back({cls, pit->second});
                }
                for (auto& parent : cls->getSuperClasses()) {
                    gatherProps(parent);
                }
            };
        gatherProps(klass);
        if (allMatches.empty()) {
            // A value-type receiver has no static / package / late-bound surface, so
            // an unmatched name is a field typo, not a later pass's business.
            if (klass->isValueType()) {
                throw cajeta::Exception(
                    "'" + klass->getQName()->toCanonical()
                        + "' has no field '" + identifier + "'",
                    "CAJETA_ERROR_UNKNOWN_FIELD");
            }
            // We reach here only with a real instance, so an unmatched name is a field
            // typo; returning null here is what let `p.vee` compile to nothing.
            std::string msg = "no member '" + identifier + "' on '"
                + klass->getQName()->toCanonical() + "'";
            // Typed accessors do not exist on `Table<?>`: say how to proceed, not a hint.
            {
                auto origin = klass->isInstantiation()
                    ? klass->getTemplateOrigin() : nullptr;
                const auto& targs = klass->getTypeArguments();
                if (origin && origin->getQName()
                        && origin->getQName()->toCanonical()
                            == "cajeta.nucleo.frame.Table"
                        && targs.size() == 1 && targs[0]
                        && targs[0]->isWildcard()
                        && !targs[0]->wildcardBound()) {
                    throw locatedException(
                        getSourceLine(), getSourceColumn() + 1,
                        msg + " — schema not statically known here; narrow "
                        "with `.as<R>()` or use `col(\"...\")`",
                        "CAJETA_ERROR_MEMBER_NOT_FOUND");
                }
            }
            std::string hint = klass->suggestMemberName(identifier);
            if (!hint.empty()) msg += " — did you mean '" + hint + "'?";
            throw locatedException(
                getSourceLine(), getSourceColumn() + 1, msg,
                "CAJETA_ERROR_MEMBER_NOT_FOUND");
        }
        // Self-shadow resolves ambiguity: the receiver class's own property wins.
        auto selfPit = klass->getProperties().find(identifier);
        bool selfShadows = (selfPit != klass->getProperties().end());
        if (!selfShadows && allMatches.size() > 1) {
            // Sibling collision: two declaring classes, neither an ancestor.
            std::function<bool(CajetaClassPtr, CajetaClassPtr)> isAncestor =
                [&](CajetaClassPtr anc, CajetaClassPtr desc) -> bool {
                    if (!anc || !desc) return false;
                    if (anc.get() == desc.get()) return true;
                    for (auto& sup : desc->getSuperClasses()) {
                        if (isAncestor(anc, sup)) return true;
                    }
                    return false;
                };
            for (size_t i = 0; i < allMatches.size(); ++i) {
                for (size_t j = i + 1; j < allMatches.size(); ++j) {
                    if (allMatches[i].first.get() == allMatches[j].first.get()) continue;
                    bool aIsAncOfB = isAncestor(allMatches[i].first, allMatches[j].first);
                    bool bIsAncOfA = isAncestor(allMatches[j].first, allMatches[i].first);
                    if (!aIsAncOfB && !bIsAncOfA) {
                        std::string msg = "field access '" + identifier
                            + "' on class '"
                            + klass->getQName()->toCanonical()
                            + "' is ambiguous; both '"
                            + allMatches[i].first->getQName()->toCanonical()
                            + "." + identifier + "' and '"
                            + allMatches[j].first->getQName()->toCanonical()
                            + "." + identifier
                            + "' reach this class through different parents. "
                            + "Resolve by either (1) declaring '"
                            + identifier + "' on '"
                            + klass->getQName()->toCanonical()
                            + "' to shadow both or (2) qualifying the access "
                            + "via 'this<Base>." + identifier
                            + "' (MultiClassing Phase 2)";
                        throw cajeta::Exception(msg,
                            "CAJETA_ERROR_AMBIGUOUS_FIELD_ACCESS");
                    }
                }
            }
        }
        // Self-shadow wins; otherwise the first gathered match, in declaration order.
        CajetaClassPtr pickedDeclaringClass;
        if (selfShadows) {
            lookedUpProperty = selfPit->second;
            pickedDeclaringClass = klass;
        } else {
            lookedUpProperty = allMatches.front().second;
            pickedDeclaringClass = allMatches.front().first;
        }
        // Synthesize an iterator-like pair so the `it->second` code below still works.
        std::pair<string, StructurePropertyPtr> foundEntry(identifier, lookedUpProperty);
        auto it = klass->getProperties().find(identifier);
        bool inheritedFromAncestor = (it == klass->getProperties().end());
        if (inheritedFromAncestor) {
            // Downstream reads the synthesized entry, not `it->second`.
        }
        // An l-value receiver (an alloca holding a pointer) is loaded through first:
        // GEP'ing the alloca would walk the slot, not the object.
        if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(base)) {
            // A @ValueType slot holds the aggregate INLINE, so the alloca address IS
            // the object. A value-type method's `this` is still a ptr slot, and loads.
            bool slotHoldsAggregate =
                lhs->getResolvedType() && lhs->getResolvedType()->isValueType()
                && !a->getAllocatedType()->isPointerTy();
            if (!slotHoldsAggregate) {
                base = module->getBuilder()->CreateLoad(a->getAllocatedType(), a);
            }
        } else if (llvm::isa<llvm::GetElementPtrInst>(base)) {
            // A slot GEP into a field holding a `ptr` to an instance loads through.
            // Not views (fields are inline), not this/super (already the receiver).
            auto lhsClass = dynamic_pointer_cast<CajetaClass>(lhs->getResolvedType());
            bool lhsIsView = dynamic_pointer_cast<CajetaView>(lhs->getResolvedType()) != nullptr;
            bool lhsIsThisOrSuper =
                dynamic_pointer_cast<ThisExpression>(lhs) != nullptr
                || dynamic_pointer_cast<SuperExpression>(lhs) != nullptr;
            // An inline interface body is GEP'd directly by dispatch; loading would
            // read its data ptr as a body. Interface LOCALS load in the branch above.
            bool lhsIsInterface = lhsClass && lhsClass->isInterface();
            // An inline @ValueType element: the GEP already addresses the aggregate,
            // so a load would read the struct's first word as a pointer.
            bool lhsIsValueType = lhsClass && lhsClass->isValueType();
            if (lhsClass && !lhsIsView && !lhsIsThisOrSuper && !lhsIsInterface
                    && !lhsIsValueType) {
                auto ptrTy = llvm::PointerType::get(*module->getLlvmContext(), 0);
                base = module->getBuilder()->CreateLoad(ptrTy, base);
            }
        } else if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(base)) {
            // A reference-type static field's global holds a `ptr` to the instance,
            // so load through before GEP'ing; a value type is stored inline.
            if (gv->getValueType()->isPointerTy()) {
                base = module->getBuilder()->CreateLoad(
                    llvm::PointerType::get(*module->getLlvmContext(), 0), gv);
            }
        }
        StructurePropertyPtr property = lookedUpProperty;
        // Pin our own resolvedType so callers load through with the right element
        // type; captureProject projects a bounded-wildcard receiver to its bound.
        resolvedType = CajetaType::captureProject(property->getType());

        // vbase indirection: a property declared on an ancestor of `this`'s static
        // class GEPs from klass's vbase pointer. Skipped for views and when absent.
        if (pickedDeclaringClass && klass
                && pickedDeclaringClass.get() != klass.get()
                && !dynamic_pointer_cast<CajetaView>(klass)) {
            int vbaseSlot = klass->getVbaseSlotIndex(
                pickedDeclaringClass.get());
            if (vbaseSlot >= 0) {
                auto* builder = module->getBuilder();
                llvm::Type* klassLlvm = klass->getLlvmType();
                llvm::Type* ptrTy = llvm::PointerType::get(
                    *module->getLlvmContext(), 0);
                llvm::Value* vbaseSlotPtr = builder->CreateStructGEP(
                    klassLlvm, base, (unsigned) vbaseSlot,
                    "vbase_slot");
                llvm::Value* vbasePtr = builder->CreateLoad(
                    ptrTy, vbaseSlotPtr, "vbase_load");
                base = vbasePtr;
                klass = pickedDeclaringClass;
            }
        }

        // view v1.1 descriptor unwrap: a view with element-array fields carries a
        // pointer to an arena {i8* data, i64* table}. Unwrapped once, here.
        llvm::Value* veaTable = nullptr;
        if (auto descView = dynamic_pointer_cast<CajetaView>(klass)) {
            if (descView->getHasElementArrayField() && base) {
                auto* b = module->getBuilder();
                auto& c = *module->getLlvmContext();
                llvm::PointerType* pTy = llvm::PointerType::get(c, 0);
                llvm::Value* descPtr = base;
                base = b->CreateLoad(pTy, descPtr, "vea_data");
                llvm::Value* tSlot = b->CreateInBoundsGEP(
                    llvm::Type::getInt8Ty(c), descPtr,
                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(c), 8),
                    "vea_tslot");
                veaTable = b->CreateLoad(pTy, tSlot, "vea_table");
            }
        }

        // CajetaClass instances reserve LLVM slot 0 for the vtable, so user fields sit
        // at getOrder()+1; CajetaView has no vtable and uses getOrder() directly.
        unsigned fieldIdx = (unsigned) klass->getFieldLlvmIndex(property);

        // A variable-size view field lays out as i32 length + data past the LLVM
        // struct, so the Kth of them walks K prior prefixes. View receivers only.
        // they fall through to the standard struct-GEP path below.
        auto viewType = dynamic_pointer_cast<CajetaView>(klass);
        if (viewType && CajetaView::isVariableSize(property)) {
            auto* builder = module->getBuilder();
            auto& ctx = *module->getLlvmContext();
            llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
            llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
            llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);

            // Field start offset: a descriptor view reads it from the table in O(1),
            // a plain view walks every prior property through emitAccessAdvance.
            uint64_t fixedPrefixSize = viewType->getFixedSize();
            llvm::Value* offset;
            if (veaTable) {
                int slot = viewType->tableSlotOf(property);
                offset = builder->CreateLoad(i64Ty,
                    builder->CreateInBoundsGEP(i64Ty, veaTable,
                        llvm::ConstantInt::get(i64Ty, slot),
                        identifier + "_tbl_slot"),
                    identifier + "_tbl_off");
            } else {
                offset = llvm::ConstantInt::get(i64Ty, fixedPrefixSize);
                bool sawVar = false;
                for (auto& p : viewType->getPropertyList()) {
                    if (p == property) break;
                    bool pVar = CajetaView::isVariableSize(p);
                    if (!sawVar && !pVar) continue;  // in the fixed prefix
                    if (pVar) sawVar = true;
                    offset = CajetaView::emitAccessAdvance(
                        module, p, base, offset, viewType->getEndianness());
                }
            }

            // Element-array field: a whole-field read has no materialization, so only
            // f[i] and f.count() may take the raw prefix pointer.
            if (CajetaView::isElementArray(property)) {
                if (elementArrayPrefixMode) {
                    elementArrayPrefixMode = false;
                    earrDataBase = base;
                    earrTable = veaTable;
                    earrSlot = veaTable ? viewType->tableSlotOf(property) : -1;
                    return builder->CreateInBoundsGEP(
                        i8Ty, base, offset, identifier + "_earr_prefix");
                }
                throw Exception(
                    "view element-array field '" + identifier + "' cannot be "
                    "read whole — index it (" + identifier + "[i]) or take "
                    + identifier + ".count() "
                    "(specs/view-element-arrays-spec.md)",
                    "CAJETA_ERROR_VIEW_ELEMENT_ARRAY_BARE_READ");
            }

            llvm::Value* prefixPtr = builder->CreateInBoundsGEP(
                i8Ty, base, offset, identifier + "_len_ptr");
            llvm::Value* length = CajetaView::emitSwapIfNeeded(
                module, viewType->getEndianness(),
                builder->CreateLoad(i32Ty, prefixPtr, identifier + "_len"));
            llvm::Value* length64 = builder->CreateIntCast(
                length, i64Ty, /*isSigned=*/true);
            llvm::Value* dataOffset = builder->CreateAdd(
                offset, llvm::ConstantInt::get(i64Ty, 4),
                identifier + "_data_offset");
            llvm::Value* dataPtr = builder->CreateInBoundsGEP(
                i8Ty, base, dataOffset, identifier + "_data");

            // Materialize an owned value: String → UTF-8 copy, T[] → fresh heap array.
            auto fieldQn = property->getType()->getQName();
            if (fieldQn && fieldQn->getTypeName() == "String") {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_view_to_owned");
                if (!fn) return nullptr;
                // String is a CLASS, so the helper's malloc'd char* is wrapped in a
                // String instance; otherwise dispatch reads the payload as a vtable.
                llvm::Value* cstr = builder->CreateCall(fn, {dataPtr, length64});
                return wrapCStringIntoClassString(module, cstr, identifier.c_str());
            }
            if (auto arrType = dynamic_pointer_cast<CajetaArray>(property->getType())) {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_array_view_to_owned");
                if (!fn) return nullptr;
                // Element size drives the runtime's total memcpy byte count.
                uint64_t elemBytes = 1;
                if (auto elemTy = arrType->getElementLlvmType(&ctx)) {
                    elemBytes = module->getLlvmModule()->getDataLayout()
                        .getTypeAllocSize(elemTy);
                    if (elemBytes == 0) elemBytes = 1;
                }
                llvm::Value* elemBytesConst = llvm::ConstantInt::get(
                    i64Ty, elemBytes);
                return builder->CreateCall(fn, {dataPtr, length64, elemBytesConst});
            }
            return dataPtr;
        }

            // A post-variable fixed field has no LLVM-struct slot: walk the preceding
            // var-size prefixes at runtime. Pre-variable ones GEP below.
        if (auto viewType = dynamic_pointer_cast<CajetaView>(klass)) {
            // No preceding var-size field means the standard path handles it.
            int priorVarSize = 0;
            bool isPostVariable = false;
            for (auto& p : viewType->getPropertyList()) {
                if (p == property) {
                    isPostVariable = (priorVarSize > 0);
                    break;
                }
                if (CajetaView::isVariableSize(p)) priorVarSize += 1;
            }
            if (isPostVariable) {
                auto* builder = module->getBuilder();
                auto& ctx = *module->getLlvmContext();
                llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
                llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
                llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);

                // Descriptor view: the absolute offset is in the table, with no walk.
                if (veaTable) {
                    int slot = viewType->tableSlotOf(property);
                    llvm::Value* tOff = builder->CreateLoad(i64Ty,
                        builder->CreateInBoundsGEP(i64Ty, veaTable,
                            llvm::ConstantInt::get(i64Ty, slot),
                            identifier + "_tbl_slot"),
                        identifier + "_tbl_off");
                    return builder->CreateInBoundsGEP(
                        i8Ty, base, tOff, identifier + "_ptr");
                }

                uint64_t fixedPrefixSize = viewType->getFixedSize();
                llvm::Value* offset = llvm::ConstantInt::get(i64Ty, fixedPrefixSize);

                // Walk every property up to THIS one, tracking the running offset.
                bool sawVar = false;
                for (auto& p : viewType->getPropertyList()) {
                    if (p == property) break;
                    bool pVar = CajetaView::isVariableSize(p);
                    if (!sawVar && !pVar) continue;
                    // Pre-var fixed fields are already in fixedPrefixSize.
                    if (pVar) sawVar = true;
                    offset = CajetaView::emitAccessAdvance(
                        module, p, base, offset, viewType->getEndianness());
                }

                llvm::Value* fieldPtr = builder->CreateInBoundsGEP(
                    i8Ty, base, offset, identifier + "_ptr");
                // An l-value pointer; the caller's loadIfLValue does the typed load.
                return fieldPtr;
            }
        }

        llvm::Value* fieldGep = module->getBuilder()->CreateStructGEP(
            klass->getLlvmType(), base, fieldIdx, identifier);
        // TBAA: the disjoint "field" tag lets the optimizer hoist field loads across
        // array-element stores — array buffers and object storage never overlap.
        module->recordTbaaProvenance(fieldGep, CajetaModule::TbaaKind::Field);
        return fieldGep;
    }

} // code
