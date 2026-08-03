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
    DotExpression::DotExpression(CajetaParser::ExpressionContext* ctx, antlr4::Token* token) : Expression(token) {
        // The DOT grammar allows several rhs forms (identifier, methodCall, THIS, etc.).
        // Only the identifier form is fully implemented; for other forms we capture an
        // empty name and rely on the lhs's codegen to surface the right error.
        if (ctx->identifier()) {
            identifier = ctx->identifier()->getText();
            if (auto* idTok = ctx->identifier()->getStart()) {
                idLine   = (int) idTok->getLine();
                idColumn = (int) idTok->getCharPositionInLine();
            }
        }
    }

    void DotExpression::resolveTypes(CajetaModulePtr module) {
        // Resolve the LHS (instance expression) first, then look up our member on its
        // resolved class type and pin our own type to the member's type.
        AbstractSyntaxNode::resolveTypes(module);
        if (children.empty()) {
            return;
        }
        auto lhs = dynamic_pointer_cast<Expression>(children[0]);
        if (!lhs) {
            return;
        }
        // Enum constant reference: `MyEnum.NAME` — resolvedType is int32.
        // Bind here so diamond inference and other resolveType consumers see
        // the right type without waiting for generateCode to short-circuit.
        if (auto id = dynamic_pointer_cast<IdentifierExpression>(lhs)) {
            const string& ns = id->getTextValue();
            if (CajetaType::lookupEnumConstant(ns, identifier).has_value()) {
                // Enum constant `MyEnum.NAME` — resolvedType is int32 (the
                // ordinal's type, which generateCode emits). NOTE: keeping this
                // int32 (rather than the enum type) is deliberate — resolving it
                // to the enum type broke enum `==`/codegen across the stdlib.
                // The int32↔enum overload-resolution gap (an enum-constant arg
                // failing to match an enum-typed parameter) is closed in
                // CajetaClass::subtypeDistance instead.
                resolvedType = CajetaType::of("int32");
                return;
            }
            // Static field reference: `Counter.total`. LHS names a class
            // (it isn't a local variable). Pin LHS's resolvedType to the
            // class and our own resolvedType to the field's declared
            // type. Resolved through ofScoped (own package → imports →
            // global) — the raw short-name key is last-writer-wins across
            // packages, so a same-named class elsewhere would hijack the
            // reference. Falling through to the instance-path below would
            // set klass=null because IdentifierExpression doesn't resolve
            // class names by default.
            if (!lhs->getResolvedType()) {
                auto scoped = CajetaType::ofScoped(ns, module);
                if (auto staticKlass = dynamic_pointer_cast<CajetaClass>(scoped)) {
                    lhs->setResolvedType(staticKlass);
                }
            }
        }
        // Vector component access: v.x/.y/.z/.w (and .r/.g/.b/.a) -> element
        // type. Invalid components are left unresolved here; generateCode emits
        // the clear diagnostic.
        if (auto vecT = dynamic_pointer_cast<CajetaVector>(lhs->getResolvedType())) {
            int lane = vecops::laneForComponentName(identifier);
            if (lane >= 0 && (unsigned) lane < vecT->getLanes()) {
                resolvedType = vecT->getElementType();      // single component
            } else {
                // Multi-component swizzle (`.xy`/`.xyz`/`.xxyy`) -> Vector<T,M>.
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
        // properties maps, not on the subclass's own. Mirrors the lookup
        // in generateCode below.
        std::function<bool(const CajetaClassPtr&)> findProp =
            [&](const CajetaClassPtr& cls) -> bool {
                auto pit = cls->getProperties().find(identifier);
                if (pit != cls->getProperties().end()) {
                    // Capture conversion: field read through a bounded-
                    // wildcard receiver projects to the bound so chained
                    // member lookup works (P2-2 item 1; mirrors the MCE
                    // return-type pin).
                    resolvedType = CajetaType::captureProject(
                        pit->second->getType());
                    // xref (2.1.6): `cls` — not `klass` — is the class that actually
                    // DECLARES this field. For an inherited field the two differ, and
                    // naming the receiver's class would point Ctrl-click at a type
                    // that declares nothing of the sort.
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
    // unless --emit-xref, and skipped entirely when this node came from synthesized
    // source (getSourceFile() empty) — a snippet's positions are not anywhere.
    void DotExpression::recordFieldXref(const CajetaClassPtr& owner) {
        if (!xref::captureEnabled() || !owner || idLine <= 0) return;
        const string& file = getSourceFile();
        if (file.empty()) return;

        // An instantiation (`Box<int32>`) has no source; its field is declared on
        // the template, which is what the index carries.
        string ownerFqn = owner->getQName()->toCanonical();
        auto lt = ownerFqn.find('<');
        if (lt != string::npos) ownerFqn = ownerFqn.substr(0, lt);

        xref::noteFieldReference(ownerFqn + "." + identifier, file, idLine, idColumn);
    }

    // `a.b` lowers to a struct GEP. lhs may be the alloca holding the struct (stack-local)
    // or a pointer loaded from the heap; both yield an address we can GEP into. The member
    // index comes from StructureProperty::getOrder() — set when the class registered its
    // properties during signature pass.
    llvm::Value* DotExpression::maybeBswap(CajetaModulePtr module, llvm::Value* v,
                                              const ExpressionPtr& receiver) {
        if (!v || !receiver) return v;
        auto recvType = receiver->getResolvedType();
        if (!recvType) return v;
        // Endianness is view-only — structs are host-endian per docs/specification/lang/Views.md.
        // Cast specifically to CajetaView so a future struct receiver no-ops
        // through here.
        auto viewType = dynamic_pointer_cast<CajetaView>(recvType);
        if (!viewType) return v;
        ViewEndianness e = viewType->getEndianness();
        if (e == ViewEndianness::Host) return v;
        // v1 assumption: host is little-endian (x86_64, aarch64). When we
        // grow cross-compile support, this picks the host's order from the
        // target triple instead.
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

    llvm::Value* DotExpression::generateCode(CajetaModulePtr module) {
        if (children.empty()) {
            return nullptr;
        }

        // A transferred PATH is readable, exactly as a transferred identifier
        // is: `#person.name` demotes `person.name` to a borrow of the same
        // live instance rather than killing the path. Re-transferring it is
        // rejected at the `#` operand instead.
        // Per `specs/transfer-demotes-to-borrow-spec.md` §2.3.1.

        // Static-namespace constants: Math.PI / Math.E / Integer.MAX_VALUE / ... .
        // These have no instance backing and don't survive the GEP path below, so we
        // short-circuit them here and emit IR constants directly.
        if (auto idExpr = dynamic_pointer_cast<IdentifierExpression>(children[0])) {
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
            // Enum constant: `MyEnum.NAME` resolves to the ordinal i32. The
            // enum type is registered in canonicalMap; the constant table is
            // a separate side-map populated by visitEnumDeclaration.
            if (auto v = CajetaType::lookupEnumConstant(ns, identifier)) {
                // Type the constant as the ENUM rather than raw int32, so a
                // method invoked directly on it (`Verb.POST.weight()`) can
                // reach the enum's companion class. The VALUE is unchanged —
                // still the ordinal.
                auto& cmap = CajetaType::getCanonicalMap();
                auto et = cmap.find(ns);
                if (et != cmap.end()) {
                    resolvedType = et->second;
                }
                return llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(ctx), *v, /*isSigned=*/true);
            }
        }

        // Static-field-on-class-name shortcut: `Counter.total`. LHS is a
        // bare identifier naming a class (not a local). The instance
        // GEP path below assumes LHS resolves to an instance and would
        // bail at `if (!base)` because IdentifierExpression returns
        // null for class names. Route to the class's static-field
        // global instead — returned as an l-value pointer so
        // loadIfLValue handles reads and BinaryOpExpression's assign
        // path handles writes.
        if (auto idLhs = dynamic_pointer_cast<IdentifierExpression>(children[0])) {
            // ofScoped, not the raw short-name key (see resolveTypes above).
            auto scoped = CajetaType::ofScoped(idLhs->getTextValue(), module);
            {
                if (auto staticKlass = dynamic_pointer_cast<CajetaClass>(scoped)) {
                    // Walk the hierarchy: a static declared on a base
                    // class is visible through derived-class names too.
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
        // Re-run resolveTypes if the lhs wasn't resolved during the pre-pass —
        // local variables aren't added to the scope until their declarations
        // run at codegen time, so identifiers referenced later may have a
        // null resolvedType at resolve time.
        if (!lhs->getResolvedType()) {
            lhs->resolveTypes(module);
        }
        // Vector component read: v.x/.y/.z/.w (and .r/.g/.b/.a) -> extractelement.
        if (auto vecT = dynamic_pointer_cast<CajetaVector>(lhs->getResolvedType())) {
            int lane = vecops::laneForComponentName(identifier);
            if (lane >= 0) {
                // Single component `.x` -> the element (extractelement).
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
            // Multi-component swizzle `.xy`/`.xyz`/`.xxyy` -> Vector<T,M>.
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
        // Walk the inheritance chain to find the property. The subclass's
        // own `properties` map only contains its own declared fields;
        // inherited fields live on the parent's map. Property indices
        // returned by getFieldLlvmIndex are valid for any descendant's
        // LLVM struct because subclass structs prepend parent fields at
        // the parent's indices.
        //
        // MultiClassing Phase 1 (P-1): walk the FULL parent set
        // (not first-match-wins), gather every class that declares a
        // property by this name, and reject when the gathered set
        // contains two sibling classes (no inheritance relationship)
        // and the receiver class itself doesn't shadow the name.
        // `getProperties()` is per-declaring-class — inherited fields
        // are NOT in the subclass's map — so a self-shadow shows up
        // as `klass->getProperties().find(identifier) != end()`.
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
            // A value-type (record / @ValueType) receiver has no static/
            // package/late-bound member surface — an unmatched name is a
            // field typo, not something a later resolution pass can claim
            // (records-spec §5.2). Reference classes keep the silent
            // fall-through their qualified-name resolution relies on.
            if (klass->isValueType()) {
                throw cajeta::Exception(
                    "'" + klass->getQName()->toCanonical()
                        + "' has no field '" + identifier + "'",
                    "CAJETA_ERROR_UNKNOWN_FIELD");
            }
            // Reference class (2.2.3). We reach here only with a real instance
            // (`base` is non-null, guarded above), so the receiver is not a bare
            // type name — statics / enum constants / qualified names bail out
            // earlier and keep their fall-through. An unmatched name on an
            // INSTANCE is a field typo, and returning null here is what let
            // `p.vee` compile to nothing.
            std::string msg = "no member '" + identifier + "' on '"
                + klass->getQName()->toCanonical() + "'";
            // The frame's schema-erased table (spec §4.3.2 wording
            // contract): typed accessors don't exist on `Table<?>` — say
            // exactly how to proceed instead of offering a spelling hint.
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
        // Self-shadow resolves ambiguity. Take the receiver class's
        // own property if it declared one.
        auto selfPit = klass->getProperties().find(identifier);
        bool selfShadows = (selfPit != klass->getProperties().end());
        if (!selfShadows && allMatches.size() > 1) {
            // Check for sibling collision: two declaring classes
            // where neither is ancestor of the other.
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
        // Pick the property — self-shadow wins; otherwise the first
        // gathered match (DFS / declaration order) is the one to use.
        // Common-ancestor case (multiple matches but all collapse to
        // one declaring class) reaches here cleanly.
        CajetaClassPtr pickedDeclaringClass;
        if (selfShadows) {
            lookedUpProperty = selfPit->second;
            pickedDeclaringClass = klass;
        } else {
            lookedUpProperty = allMatches.front().second;
            pickedDeclaringClass = allMatches.front().first;
        }
        // Synthesize an iterator-like pair so the existing `it->second`
        // code below continues to work unchanged. The found property is
        // what we'd have gotten from a direct map lookup.
        std::pair<string, StructurePropertyPtr> foundEntry(identifier, lookedUpProperty);
        auto it = klass->getProperties().find(identifier);
        bool inheritedFromAncestor = (it == klass->getProperties().end());
        if (inheritedFromAncestor) {
            // Use the synthesized entry so downstream code treats the
            // inherited property the same as an own one. (it->second is
            // referenced below; emulate it with the looked-up property.)
        }
        // If the receiver is an l-value (an alloca that holds a pointer to the
        // object), load through it first. The struct/class instance lives at
        // the address the alloca stores; GEP'ing the alloca directly would
        // walk the slot, not the object.
        if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(base)) {
            // @ValueType receiver whose slot holds the aggregate INLINE
            // (StackField allocates `alloca %ValueType`): the alloca address IS
            // the object, so GEP it directly — loading would yield the aggregate
            // VALUE, which can't be a GEP base. The allocated-type check is
            // load-bearing: a value-type METHOD's `this` is still a POINTER
            // spilled to an `alloca ptr` slot (receiver passed by reference even
            // for value types), so that case must load through to the object.
            // Reference types likewise keep a `ptr` slot → load through.
            bool slotHoldsAggregate =
                lhs->getResolvedType() && lhs->getResolvedType()->isValueType()
                && !a->getAllocatedType()->isPointerTy();
            if (!slotHoldsAggregate) {
                base = module->getBuilder()->CreateLoad(a->getAllocatedType(), a);
            }
        } else if (llvm::isa<llvm::GetElementPtrInst>(base)) {
            // Chained class-field access (`foo.bar.value`) or
            // implicit-this class-typed field access (`t.v` inside
            // a method where `t` is `this.t`). The previous step's
            // generateCode returned a slot GEP into a struct field
            // that holds a `ptr` to a class instance — not the
            // instance pointer itself. Load through to dereference.
            //
            // The guard:
            //   - resolvedType must be a CajetaClass (covers
            //     interface too — CajetaInterface extends
            //     CajetaClass).
            //   - It must NOT be a CajetaView, since view fields
            //     are stored INLINE in the byte buffer and the GEP
            //     already gives the field's address directly.
            //   - It must NOT be a ThisExpression or SuperExpression
            //     LHS (Phase 2 / Phase 3 v2): those primaries return a
            //     ready-to-use instance pointer (potentially adjusted
            //     by `adjustForUpcast` for the bracketed `this<Base>` /
            //     `super<Base>` form). The pointer they hand back is
            //     the receiver itself, not a slot-holding-pointer. A
            //     spurious load-through here reads garbage at the
            //     adjusted offset and downstream GEPs build on it.
            //
            // CajetaArray fields also store via pointer indirection
            // but DotExpression on an array receiver isn't a
            // supported shape in v1 (arrays go through the index
            // expression path), so leaving them out doesn't open
            // a new gap here.
            auto lhsClass = dynamic_pointer_cast<CajetaClass>(lhs->getResolvedType());
            bool lhsIsView = dynamic_pointer_cast<CajetaView>(lhs->getResolvedType()) != nullptr;
            bool lhsIsThisOrSuper =
                dynamic_pointer_cast<ThisExpression>(lhs) != nullptr
                || dynamic_pointer_cast<SuperExpression>(lhs) != nullptr;
            // Interface receivers are 24-byte fat-pointer bodies stored INLINE
            // (an interface array element / field), so the GEP already points AT
            // the body — the interface dispatch path (CajetaClass.cpp) GEPs
            // data/vtable straight from it. Loading through here would read the
            // body's first word (the data ptr) and dispatch would then deref that
            // as a body → garbage vtable → crash. (Interface LOCALS are AllocaInst
            // slots holding a ptr-to-body, loaded correctly by the alloca branch
            // above; only the inline GEP shape must skip the load.)
            bool lhsIsInterface = lhsClass && lhsClass->isInterface();
            // A @ValueType receiver is stored INLINE (e.g. a value-type element
            // of an inline `Point[N]` field, or of a heap `Point[]` data
            // region): the GEP already addresses the aggregate, so loading it
            // would read the struct's first word as a pointer and crash. Mirror
            // the alloca branch's `slotHoldsAggregate` guard. Reference-class
            // elements ARE pointer slots and still need the load-through.
            bool lhsIsValueType = lhsClass && lhsClass->isValueType();
            if (lhsClass && !lhsIsView && !lhsIsThisOrSuper && !lhsIsInterface
                    && !lhsIsValueType) {
                auto ptrTy = llvm::PointerType::get(*module->getLlvmContext(), 0);
                base = module->getBuilder()->CreateLoad(ptrTy, base);
            }
        } else if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(base)) {
            // Static-field receiver (`log.name` where `log` is a static field):
            // Identifier returns the field's GlobalVariable. A reference-type
            // static field's global holds a `ptr` to the instance, so load
            // through to the object before GEP'ing its field — the field-access
            // twin of the static-field method-receiver load. A value-type static
            // field stores its aggregate inline, so the global's address IS the
            // object: GEP it directly.
            if (gv->getValueType()->isPointerTy()) {
                base = module->getBuilder()->CreateLoad(
                    llvm::PointerType::get(*module->getLlvmContext(), 0), gv);
            }
        }
        StructurePropertyPtr property = lookedUpProperty;
        // Set our own resolvedType so callers can load-through with the right
        // element type. The pre-pass resolveTypes can't always determine this
        // (locals aren't in scope until their declarations run at codegen).
        // captureProject handles bounded-wildcard receivers — `Box<? extends
        // Animal>.value` projects to Animal so chained member lookup works.
        resolvedType = CajetaType::captureProject(property->getType());

        // MultiClassing Phase 3 v4 vbase indirection. When the property
        // is declared on an ancestor of `this`'s static class (not on
        // klass itself), load `klass`'s vbase pointer for the declaring
        // class and use it as the base for the field GEP. Replaces the
        // earlier v2 cross-path-offset trick with a universal mechanism:
        // diamond and non-diamond cases both work because each class's
        // ctor sets vbases to point at inline ancestor positions and
        // diamond descendants overwrite non-first parents' vbase slots
        // to canonical positions (CajetaClass.cpp + Method.cpp).
        //
        // Skipped for view types (CajetaView keeps inline storage with
        // no vbase machinery) and for cases where the receiver class
        // doesn't have a vbase slot for the declaring class — falls
        // back to direct GEP (own field, or single-inheritance case
        // where the layout walker confirmed no vbase needed).
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

        // view v1.1 descriptor unwrap: a view with element-array fields
        // carries its value as a pointer to an arena {i8* data, i64* table}
        // pair. Unwrap once — every access below (fixed StructGEP, var-size,
        // post-var) then works on the real data pointer, and var-size
        // offsets come from the table (O(1), no walk).
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

        // Field index depends on the receiver type. CajetaClass instances
        // reserve LLVM slot 0 for the vtable pointer, so user fields land at
        // index getOrder()+1. CajetaView (no vtable) uses getOrder() directly.
        unsigned fieldIdx = (unsigned) klass->getFieldLlvmIndex(property);

        // Variable-size view fields (`String`, `T[]`) lay out in the wire
        // format as i32 length + data bytes, all sitting past the LLVM
        // struct's footprint. For the Kth variable-size field (0-indexed)
        // we walk K prior length-prefixes at runtime to find this field's
        // own prefix position. See Views.md § Variable-size fields.
        //
        // This branch fires only for view receivers. `isVariableSize`
        // also returns true for class fields of type String, but those
        // are normal heap pointers in a vtable-prefixed class layout —
        // they fall through to the standard struct-GEP path below.
        auto viewType = dynamic_pointer_cast<CajetaView>(klass);
        if (viewType && CajetaView::isVariableSize(property)) {
            auto* builder = module->getBuilder();
            auto& ctx = *module->getLlvmContext();
            llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
            llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
            llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);

            // Field start offset. Descriptor views (element-array fields
            // present) read it from the offset table in O(1); plain views
            // walk every property before this one in declaration order —
            // emitAccessAdvance handles each kind: String (4+len),
            // primitive T[] (4+count*sizeof(T) — the old 4+count walk was
            // correct only for int8[]), and fixed-between-var fields
            // (+static size).
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

            // Element-array field (view v1.1): whole-field reads have no
            // materialization (elements are views into the buffer, not
            // copyable values). The two legitimate consumers — f[i]
            // (ArrayIndexExpression) and f.count() (MethodCallExpression) —
            // set the one-shot prefix mode and take the raw prefix pointer.
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

            // Materialize an owned value via the right runtime helper
            // for the field's element type. String → null-terminated UTF-8
            // copy. T[] → fresh heap array (header + memcpy of data bytes).
            auto fieldQn = property->getType()->getQName();
            if (fieldQn && fieldQn->getTypeName() == "String") {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_view_to_owned");
                if (!fn) return nullptr;
                // The runtime helper returns a malloc'd null-terminated
                // char* — the bytes copied out of the wire buffer. Post
                // Phase 2b-β String is a CLASS, so we need to materialize
                // a class String instance around those bytes (vtable +
                // CajetaArray header + mode=0 owned + cachedCpLength=-1)
                // so subsequent `.equals` / `.size` / etc. dispatch
                // correctly. Without this the caller would treat the
                // raw char* as a class String pointer, vtable-dispatch
                // would load the first 8 bytes of the payload as the
                // vtable, and crash on bogus function-pointer addresses.
                llvm::Value* cstr = builder->CreateCall(fn, {dataPtr, length64});
                return wrapCStringIntoClassString(module, cstr, identifier.c_str());
            }
            if (auto arrType = dynamic_pointer_cast<CajetaArray>(property->getType())) {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_array_view_to_owned");
                if (!fn) return nullptr;
                // Element size from the array's element LLVM type. Required
                // by the runtime to compute total byte count for memcpy.
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

        // Post-variable fixed field access (S5b). A view may now declare
        // fixed-size fields after a variable-size field. Their LLVM-struct
        // slot doesn't exist; we walk all preceding var-size length-prefixes
        // at runtime to find this field's offset, then GEP from `base`.
        //
        // The pre-variable fixed fields fall through to the standard
        // CreateStructGEP path below — their offsets are compile-time-constant
        // and live in the LLVM struct.
        if (auto viewType = dynamic_pointer_cast<CajetaView>(klass)) {
            // Find this property's position in propertyList AND count
            // var-size fields that precede it. If there are no preceding
            // var-size fields, the field is pre-variable and the standard
            // path handles it.
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

                // Descriptor view: the field's absolute offset is in the
                // table — O(1), no walk over preceding var-size fields.
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

                // Walk every property up to (not including) THIS field,
                // tracking the running offset. emitAccessAdvance handles
                // every kind — String, primitive T[] (count*sizeof(T)),
                // fixed-between-var, and element arrays (runtime loop) —
                // so post-var fixed fields are reachable across all of
                // them (view v1.1).
                bool sawVar = false;
                for (auto& p : viewType->getPropertyList()) {
                    if (p == property) break;
                    bool pVar = CajetaView::isVariableSize(p);
                    if (!sawVar && !pVar) continue;
                    // Pre-var fixed fields contribute to fixedPrefixSize
                    // already (handled by the starting offset).
                    if (pVar) sawVar = true;
                    offset = CajetaView::emitAccessAdvance(
                        module, p, base, offset, viewType->getEndianness());
                }

                llvm::Value* fieldPtr = builder->CreateInBoundsGEP(
                    i8Ty, base, offset, identifier + "_ptr");
                // Return as an l-value pointer; caller's loadIfLValue will
                // load through with the right element type. Mirrors what
                // CreateStructGEP returns in the standard path.
                return fieldPtr;
            }
        }

        llvm::Value* fieldGep = module->getBuilder()->CreateStructGEP(
            klass->getLlvmType(), base, fieldIdx, identifier);
        // TBAA: object-field access. The disjoint "field" tag lets the optimizer
        // hoist field loads across array-element stores (which carry the
        // array-element tag) — array buffers and object storage never overlap.
        module->recordTbaaProvenance(fieldGep, CajetaModule::TbaaKind::Field);
        return fieldGep;
    }

} // code
