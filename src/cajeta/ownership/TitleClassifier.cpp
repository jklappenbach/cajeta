//
// ownership-title-classifier — the one classifier (see the header).
//
// Every rule here is a row of spec §2.1 / §2.3 and is pinned by
// test/ownership/TitleClassifierTests.cpp through the audit. Consumers do
// not re-derive any of it; they ask policy() for their role.
//

#include "TitleClassifier.h"

#include "cajeta/asn/expression/AggregateInitializerExpression.h"
#include "cajeta/asn/expression/BinaryOpExpression.h"
#include "cajeta/asn/expression/CallExpression.h"
#include "cajeta/asn/expression/DotExpression.h"
#include "cajeta/asn/expression/Identifier.h"
#include "cajeta/asn/expression/LiteralExpression.h"
#include "cajeta/asn/expression/MethodCallExpression.h"
#include "cajeta/asn/expression/NewExpression.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/error/Exception.h"
#include "cajeta/field/Field.h"
#include "cajeta/field/ParameterField.h"
#include "cajeta/method/Method.h"
#include "cajeta/type/CajetaArray.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaFunctionType.h"
#include "cajeta/type/CajetaView.h"
#include "cajeta/type/FormalParameter.h"
#include "cajeta/type/Scope.h"

#include "llvm/IR/IRBuilder.h"

namespace cajeta::ownership {

    namespace {

        bool g_auditEnabled = false;
        std::vector<TitleShapeRecord> g_records;

        // The type predicates the six sites used to spell out one by one.
        inline bool isLangString(const CajetaTypePtr& t) {
            auto cls = std::dynamic_pointer_cast<CajetaClass>(t);
            return cls && cls->getQName()
                && cls->getQName()->getTypeName() == "String"
                && cls->getQName()->getPackageName() == "cajeta.lang";
        }

        // An ownership-less scalar: PRIMITIVE_FLAG without being an array
        // (arrays carry the flag in this type system but are droppable
        // buffers — the `int8[]` consuming-native hazard).
        inline bool isOwnershipLessScalar(const CajetaTypePtr& t) {
            return t && (t->getTypeFlags() & PRIMITIVE_FLAG)
                && !std::dynamic_pointer_cast<CajetaArray>(t);
        }

        inline bool isReferenceCast(const CajetaTypePtr& dt) {
            auto dc = std::dynamic_pointer_cast<CajetaClass>(dt);
            auto dv = std::dynamic_pointer_cast<CajetaView>(dt);
            return (bool) dv || (dc && !dc->isInterface() && !dc->isValueType());
        }

        inline uint32_t typeFlags(const CajetaTypePtr& t) {
            uint32_t f = 0;
            if (!t) return f;
            if (isLangString(t)) f |= TitleShape::kString;
            if (std::dynamic_pointer_cast<CajetaArray>(t)) f |= TitleShape::kArray;
            if (std::dynamic_pointer_cast<CajetaView>(t)) f |= TitleShape::kView;
            if (auto cls = std::dynamic_pointer_cast<CajetaClass>(t)) {
                if (cls->isInterface()) f |= TitleShape::kInterface;
                if (cls->isValueType()) f |= TitleShape::kValue;
            }
            if (std::dynamic_pointer_cast<CajetaFunctionType>(t)) f |= TitleShape::kFunction;
            return f;
        }

        inline TitleShape make(TitleFamily fam, TitleAnswer ans, TitleSource src,
                               const ExpressionPtr& leaf, uint32_t flags = 0) {
            TitleShape s;
            s.family = fam;
            s.answer = ans;
            s.source = src;
            s.flags = flags;
            s.label = labelOfFamily(fam);
            s.leaf = leaf;
            return s;
        }

        // The join of two arms (spec §2.1, Conditional): equal static answers
        // fold to that answer; anything else is decided by the arm phi.
        inline TitleShape join(const TitleShape& a, const TitleShape& b,
                               const ExpressionPtr& leaf) {
            TitleShape s = make(TitleFamily::Conditional, TitleAnswer::Runtime,
                                TitleSource::ArmPhi, leaf,
                                (uint32_t) ((a.flags | b.flags)
                                            & (TitleShape::kString | TitleShape::kArray
                                               | TitleShape::kInterface | TitleShape::kValue
                                               | TitleShape::kView)));
            // Borrow, StackBound and Scalar all mean "no title": two such arms
            // fold to Borrow; two Owned arms to Owned; anything else is the phi.
            auto hasTitle = [](TitleAnswer x) {
                return x == TitleAnswer::Owned || x == TitleAnswer::Runtime;
            };
            if (!hasTitle(a.answer) && !hasTitle(b.answer)) {
                s.answer = TitleAnswer::Borrow;
                s.source = TitleSource::None;
            } else if (a.answer == TitleAnswer::Owned && b.answer == TitleAnswer::Owned) {
                s.answer = TitleAnswer::Owned;
                s.source = TitleSource::None;
            }
            return s;
        }

        // A read of a value that already has an owner — the same answer for a
        // literal, `this`, a field, an element: Borrow.
        inline TitleShape read(TitleFamily fam, const ExpressionPtr& leaf) {
            return make(fam, TitleAnswer::Borrow, TitleSource::None, leaf,
                        typeFlags(leaf->getResolvedType()));
        }

        inline TitleShape scalar(const ExpressionPtr& leaf) {
            return make(TitleFamily::Scalar, TitleAnswer::Scalar, TitleSource::None, leaf);
        }

        inline ExpressionPtr childOf(const ExpressionPtr& e, size_t i) {
            auto& ch = e->getChildren();
            return i < ch.size() ? std::dynamic_pointer_cast<Expression>(ch[i]) : nullptr;
        }

        // The named slot: local, formal, or nothing (a class name, a static).
        TitleShape localRead(const ExpressionPtr& leaf, const CajetaModulePtr& module) {
            auto id = std::static_pointer_cast<IdentifierExpression>(leaf);
            const std::string& name = id->getTextValue();
            // `this` spelled as an identifier (the receiver's formal) is the
            // receiver read, not a local.
            if (name == "this") return read(TitleFamily::ThisRead, leaf);
            TitleShape s = make(TitleFamily::LocalRead, TitleAnswer::Borrow,
                                TitleSource::None, leaf);
            FieldPtr f;
            if (auto sc = module->getScopeStack().peek()) f = sc->getField(name);
            if (!f) {
                s.flags |= typeFlags(leaf->getResolvedType());
                return s;
            }
            s.field = f.get();
            s.flags |= typeFlags(f->getType());
            if (f->getDropEntry()) s.flags |= TitleShape::kHasEntry;
            // An entry armed at run time (from a callee's flag or a plain
            // formal's word bit) must be READ; a statically armed one is a
            // constant title.
            if (f->isRuntimeConditionalOwner()) s.flags |= TitleShape::kRuntimeOwner;
            // A `stack` instance: its body is the frame's; a move of it
            // carries no title (spec 5.11) — kStack, checked before the
            // entry (a stack local's entry is the field-walking stack drop).
            if (f->isStackInstance()) s.flags |= TitleShape::kStack;
            if (!f->getCallBorrowOrigin().empty() || !f->getParamBorrowOrigin().empty()) {
                s.flags |= TitleShape::kBorrowOrigin;
            }
            if (auto pf = std::dynamic_pointer_cast<ParameterField>(f)) {
                s.flags |= TitleShape::kIsParam;
                auto fp = pf->getFormalParameter();
                if (fp && fp->isTransferred()) s.flags |= TitleShape::kTransferredParam;
                if (auto m = module->getCurrentMethod()) {
                    int idx = 0;
                    for (auto& p : m->getParameterList()) {
                        if (!p || p->getName() == "this") continue;
                        if (p->getName() == name) { s.paramIndex = (int8_t) idx; break; }
                        idx++;
                    }
                }
            } else if (auto m = module->getCurrentMethod()) {
                if (m->isArenaEligibleLocal(name)) s.flags |= TitleShape::kArena;
            }
            if (auto sc = module->getScopeStack().peek()) {
                if (sc->holdsStaticTitle(name)) s.flags |= TitleShape::kStaticTitle;
            }
            return s;
        }

        // What a call through a FUNCTION TYPE hands back — a closure call, or
        // a method-call-shaped invocation of a function-typed local or
        // property (`maker()`, `c.supplier()`): a by-value struct (sret) is
        // StackBound, a class pointer rides the synthesized callee's flag,
        // anything else has no title. The declaration's former M5b rule.
        TitleShape closureCallShape(const CajetaFunctionTypePtr& fnTy,
                                    const ExpressionPtr& leaf) {
            uint32_t flags = typeFlags(leaf->getResolvedType());
            if (fnTy && fnTy->usesSret()) {
                return make(TitleFamily::ClosureCall, TitleAnswer::StackBound, TitleSource::None,
                            leaf, (uint32_t) (flags | TitleShape::kStack));
            }
            if (fnTy && !std::dynamic_pointer_cast<CajetaClass>(fnTy->getReturnType())) {
                return scalar(leaf);
            }
            return make(TitleFamily::ClosureCall, TitleAnswer::Runtime, TitleSource::ReturnFlag,
                        leaf, flags);
        }

        // The function type a method-call-shaped invocation goes through when
        // its name is a function-typed local (bare call) or a function-typed
        // property of the receiver's class; null when it is a real method.
        CajetaFunctionTypePtr functionTypeOfCall(
                const std::shared_ptr<MethodCallExpression>& mce, const CajetaModulePtr& module) {
            const std::string& name = mce->getMethodCallName();
            auto& kids = mce->getChildren();
            if (kids.empty()) {
                if (auto sc = module->getScopeStack().peek()) {
                    if (FieldPtr f = sc->getField(name)) {
                        return std::dynamic_pointer_cast<CajetaFunctionType>(f->getType());
                    }
                }
                return nullptr;
            }
            auto recv = std::dynamic_pointer_cast<Expression>(kids[0]);
            if (!recv) return nullptr;
            if (!recv->getResolvedType()) recv->resolveTypes(module);
            auto recvCls = std::dynamic_pointer_cast<CajetaClass>(recv->getResolvedType());
            if (!recvCls) return nullptr;
            auto& props = recvCls->getProperties();
            auto it = props.find(name);
            if (it == props.end() || !it->second) return nullptr;
            return std::dynamic_pointer_cast<CajetaFunctionType>(it->second->getType());
        }

        TitleShape callResult(const ExpressionPtr& leaf, const CajetaModulePtr& module) {
            auto mce = std::static_pointer_cast<MethodCallExpression>(leaf);
            uint32_t flags = typeFlags(leaf->getResolvedType());
            // The DI intrinsic hands out the container's singleton: a borrow
            // with no callee to resolve (the declaration's A9 rule).
            if (mce->getMethodCallName() == "__cajeta_inject") {
                return make(TitleFamily::CallResult, TitleAnswer::Borrow, TitleSource::None, leaf, flags);
            }
            // After the call's own codegen its callee is exact (overloads
            // included); before it, the shallow resolution answers only on a
            // unique name+arity match.
            MethodPtr rm = mce->getResolvedMethod();
            if (!rm) rm = MethodCallExpression::resolveArgCalleeShallow(mce, module);
            if (!rm) {
                if (CajetaFunctionTypePtr fnTy = functionTypeOfCall(mce, module)) {
                    return closureCallShape(fnTy, leaf);
                }
                // Unresolvable before codegen: the callee's own flag decides
                // at run time (spec §2.1, CallResult).
                return make(TitleFamily::CallResult, TitleAnswer::Runtime,
                            TitleSource::ReturnFlag, leaf, flags);
            }
            flags |= typeFlags(rm->getReturnType());
            if (isOwnershipLessScalar(rm->getReturnType())) return scalar(leaf);
            auto withCallee = [&](TitleShape s) { s.callee = rm.get(); return s; };
            // A by-value struct return lands in the caller's frame: StackBound.
            if (rm->returnsStackValue()) {
                return withCallee(make(TitleFamily::CallResult, TitleAnswer::StackBound,
                                       TitleSource::None, leaf, (uint32_t) (flags | TitleShape::kStack)));
            }
            // A `^` view (a signature contract every override keeps), or a
            // plain method whose every return is an interior read
            // (Method::returnsInteriorView) AND whose dispatch is static — a
            // borrow of its receiver, statically, no flag to read. Through a
            // VIRTUAL call the body scan proves only the base's returns: an
            // override may ride a title out, so that stays Runtime (measured
            // 2026-09-07: `DynFrame sch = this.__schemaOf()` on the non-final
            // `Table<T>` lost its flagged entry to a static Borrow).
            auto& mods = rm->getModifiers();
            bool staticDispatch = rm->isStatic()
                || mods.count(PRIVATE) > 0 || mods.count(FINAL) > 0
                || (rm->getParent() && rm->getParent()->getModifiers().count(FINAL) > 0);
            if (rm->isReturnsView()
                    || (!rm->isReturnsOwnership() && staticDispatch && rm->returnsInteriorView())) {
                return withCallee(make(TitleFamily::CallResult, TitleAnswer::Borrow,
                                       TitleSource::None, leaf, (uint32_t) (flags | TitleShape::kView)));
            }
            bool ownedDecl = rm->isReturnsOwnership();
            if (ownedDecl) flags |= TitleShape::kOwnedDecl;
            // Every path below names the callee too: the owned-bind check
            // (§4.6) and the `#local` provenance read it off the shape.
            // (First pass set it on two of four paths and both checks went
            // silent — OwnedResultTransferTests / OwnedReturnOfBorrowTests
            // caught it, 2026-09-07.)
            // A `@Native` forwarding body and a body-less intrinsic return
            // without storing the flag (Method::emitNativeForwardingBody does a
            // bare `ret`): reading the TLS after them would be a STALE read.
            // Their declared stance is the answer. An abstract or interface
            // method dispatches to a body that does store it.
            bool storesFlag = rm->emitsReturnFlag() && rm->returnsClassPointer()
                && !rm->findAnnotation("Native")
                && (rm->getBlock() != nullptr || rm->isAbstract());
            if (storesFlag) {
                // The callee leaves its bit in the TLS, and that bit is the
                // truth for BOTH stances: a plain return may carry a title
                // (ownership §2.1, the tail-call ride), and a `#R` return may
                // carry a borrow (`return #= x` is its sanctioned escape) —
                // measured 2026-09-07 on the corpus: folding a `#R` arm to a
                // constant 1 dropped the read in Report::baseline. A static
                // Owned for `#R` callees needs a signature bit saying the
                // method has no mode-carrying return (plan 7.2.3).
                return withCallee(make(TitleFamily::CallResult, TitleAnswer::Runtime,
                                       TitleSource::ReturnFlag, leaf, flags));
            }
            // No flag emitted (a native, an intrinsic): the declared stance.
            return withCallee(make(TitleFamily::CallResult,
                                   ownedDecl ? TitleAnswer::Owned : TitleAnswer::Borrow,
                                   TitleSource::None, leaf, flags));
        }

        TitleShape moveOf(const ExpressionPtr& leaf, const CajetaModulePtr& module);

    }  // namespace

    // The total switch. No default: -Werror=switch turns an unnamed ExprKind
    // into a build failure, which is what makes the classification total.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wswitch"
#endif
    TitleShape classify(const ExpressionPtr& e0, const CajetaModulePtr& module) {
        if (!e0) return make(TitleFamily::Unsupported, TitleAnswer::Scalar, TitleSource::None, e0);
        // Peel parenthesised primaries and reference casts (spec 5.5): the
        // value is the inner expression's.
        ExpressionPtr e = e0;
        for (;;) {
            if (e->kind() == ExprKind::Primary) {
                if (auto c = childOf(e, 0)) { e = c; continue; }
                break;
            }
            if (e->kind() == ExprKind::Cast) {
                auto ce = std::static_pointer_cast<CastExpression>(e);
                if (isReferenceCast(ce->getDestType())) {
                    if (auto c = childOf(e, 0)) { e = c; continue; }
                }
                break;
            }
            break;
        }
        // The type decides several rows (a String concat, a scalar, a value
        // type): resolve it if nothing has yet — a classification before the
        // arms' resolution read a concat as a scalar (measured 2026-09-07).
        if (!e->getResolvedType()) {
            // A resolution can fail BEFORE codegen (an array literal whose
            // elements are calls the conditional has not resolved yet —
            // measured 2026-09-08 on a `#T` return's arm walk); the KIND
            // still decides the family, only the type flags go unread.
            // classify never throws.
            try {
                e->resolveTypes(module);
            } catch (Exception&) {
            }
        }
        // An ownership-less scalar is Scalar whatever produced it.
        if (isOwnershipLessScalar(e->getResolvedType())) return scalar(e);

        switch (e->kind()) {
            case ExprKind::Unsupported:
            case ExprKind::Count:
                return make(TitleFamily::Unsupported, TitleAnswer::Scalar, TitleSource::None, e);
            case ExprKind::Primary:            // a bare primary with no child
                return scalar(e);
            case ExprKind::Literal:
                return read(TitleFamily::Literal, e);
            case ExprKind::ClassLiteral:       // the runtime's cached ClassObject
                return read(TitleFamily::Literal, e);
            case ExprKind::This:
            case ExprKind::Super:
                return read(TitleFamily::ThisRead, e);
            case ExprKind::TextLiteral: {
                auto lit = std::static_pointer_cast<TextLiteralExpression>(e);
                LiteralType lt = lit->getLiteralType();
                if (lt == LITERAL_TYPE_STRING || lt == LITERAL_TYPE_TEXT_BLOCK) {
                    TitleShape s = read(TitleFamily::Literal, e);
                    s.flags |= TitleShape::kString;
                    return s;
                }
                return scalar(e);              // bool, char, null
            }
            case ExprKind::IntegerLiteral:
            case ExprKind::FloatLiteral:
            case ExprKind::InstanceOf:
            case ExprKind::Postfix:
            case ExprKind::Prefix:
            case ExprKind::Cast:               // a primitive cast (not peeled)
            case ExprKind::Detach:
                return scalar(e);
            case ExprKind::Identifier:
                return localRead(e, module);
            case ExprKind::Dot:
                return read(TitleFamily::FieldRead, e);
            case ExprKind::ArrayIndex:
                return read(TitleFamily::ElementRead, e);
            case ExprKind::ArraySlice: {
                TitleShape s = read(TitleFamily::ElementRead, e);
                s.label = "an array slice";
                return s;
            }
            case ExprKind::ArrayLiteral: {
                auto al = std::static_pointer_cast<ArrayLiteralExpression>(e);
                uint32_t f = typeFlags(e->getResolvedType()) | TitleShape::kArray;
                if (al->isStackAlloc()) f |= TitleShape::kStack;
                if (al->isArenaEligible()) f |= TitleShape::kArena;
                bool bound = al->isStackAlloc() || al->isArenaEligible();
                TitleShape s = make(TitleFamily::Fresh, bound ? TitleAnswer::StackBound : TitleAnswer::Owned,
                                    TitleSource::None, e, f);
                if (al->isStackAlloc()) s.label = "a `stack` construction";
                return s;
            }
            case ExprKind::MapLiteral:
                return make(TitleFamily::Fresh, TitleAnswer::Owned, TitleSource::None, e,
                            typeFlags(e->getResolvedType()));
            case ExprKind::Aggregate: {
                auto ag = std::static_pointer_cast<AggregateInitializerExpression>(e);
                uint32_t f = typeFlags(e->getResolvedType());
                if (ag->getStackAlloc()) {
                    TitleShape s = make(TitleFamily::Fresh, TitleAnswer::StackBound, TitleSource::None, e,
                                        (uint32_t) (f | TitleShape::kStack));
                    s.label = "a `stack` construction";
                    return s;
                }
                return make(TitleFamily::Fresh, TitleAnswer::Owned, TitleSource::None, e, f);
            }
            case ExprKind::New: {
                auto ne = std::static_pointer_cast<NewExpression>(e);
                uint32_t f = typeFlags(e->getResolvedType());
                if (ne->getStackAlloc()) {
                    TitleShape s = make(TitleFamily::Fresh, TitleAnswer::StackBound, TitleSource::None, e,
                                        (uint32_t) (f | TitleShape::kStack));
                    s.label = "a `stack` construction";
                    return s;
                }
                if (ne->getSharedAlloc()) {
                    return make(TitleFamily::Fresh, TitleAnswer::Borrow, TitleSource::None, e,
                                (uint32_t) (f | TitleShape::kShared));
                }
                return make(TitleFamily::Fresh, TitleAnswer::Owned, TitleSource::None, e, f);
            }
            case ExprKind::BinaryOp: {
                auto bo = std::static_pointer_cast<BinaryOpExpression>(e);
                BinaryOp op = bo->getBinaryOp();
                if (op == BINARY_OP_ADD && isLangString(bo->getResolvedType())) {
                    uint32_t f = TitleShape::kString;
                    if (bo->isArenaEligible()) {
                        return make(TitleFamily::Concat, TitleAnswer::StackBound, TitleSource::None,
                                    e, (uint32_t) (f | TitleShape::kArena));
                    }
                    return make(TitleFamily::Concat, TitleAnswer::Owned, TitleSource::None, e, f);
                }
                if (op == BINARY_OP_ASSIGN) {
                    // The value of an assignment is the assigned value.
                    if (auto rhs = childOf(e, 1)) return classify(rhs, module);
                }
                return scalar(e);
            }
            case ExprKind::BooleanSwitch: {
                auto a = childOf(e, 1);
                auto b = childOf(e, 2);
                if (!a || !b) return scalar(e);
                return join(classify(a, module), classify(b, module), e);
            }
            case ExprKind::Switch: {
                auto sw = std::static_pointer_cast<SwitchExpression>(e);
                bool first = true;
                TitleShape acc;
                for (auto& c : sw->getCases()) {
                    if (!c.body) continue;
                    TitleShape s = classify(c.body, module);
                    acc = first ? s : join(acc, s, e);
                    first = false;
                }
                if (first) return scalar(e);
                if (acc.family != TitleFamily::Conditional) {
                    acc = join(acc, acc, e);   // one arm: still a conditional by family
                }
                return acc;
            }
            case ExprKind::MethodCall:
                return callResult(e, module);
            case ExprKind::Call: {
                // A closure call: its function type says what comes back.
                auto ce = std::static_pointer_cast<CallExpression>(e);
                CajetaFunctionTypePtr fnTy;
                if (auto callee = ce->getCallee()) {
                    if (!callee->getResolvedType()) callee->resolveTypes(module);
                    fnTy = std::dynamic_pointer_cast<CajetaFunctionType>(callee->getResolvedType());
                }
                return closureCallShape(fnTy, e);
            }
            case ExprKind::MethodReference:
            case ExprKind::Lambda:
                return make(TitleFamily::Closure, TitleAnswer::Owned, TitleSource::None, e);
            case ExprKind::Move:
                return moveOf(e, module);
            case ExprKind::Await:              // the awaited value is the awaiter's
            case ExprKind::Spawn:              // the Task handle is the binding's
                return make(TitleFamily::Fresh, TitleAnswer::Owned, TitleSource::None, e,
                            typeFlags(e->getResolvedType()));
        }
        return make(TitleFamily::Unsupported, TitleAnswer::Scalar, TitleSource::None, e);
    }
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

    namespace {
        // `#x` / the `#=` wrapper: the inner shape decides the source.
        TitleShape moveOf(const ExpressionPtr& leaf, const CajetaModulePtr& module) {
            auto mv = std::static_pointer_cast<MoveExpression>(leaf);
            auto inner = childOf(leaf, 0);
            if (!inner) {
                TitleShape s = make(TitleFamily::Move, TitleAnswer::Owned, TitleSource::None, leaf);
                if (mv->isSharpStore()) s.flags |= TitleShape::kSharpStore;
                return s;
            }
            TitleShape s = moveFrom(classify(inner, module), mv->isSharpStore());
            s.leaf = leaf;   // the move node itself: titleFlag() then reads its stashed flag
            return s;
        }
    }  // namespace

    TitleShape moveFrom(const TitleShape& in, bool sharpStore) {
            TitleShape s = make(TitleFamily::Move, TitleAnswer::Owned, TitleSource::None, in.leaf);
            if (sharpStore) s.flags |= TitleShape::kSharpStore;
            s.flags |= in.flags;
            s.field = in.field;
            s.paramIndex = in.paramIndex;
            switch (in.family) {
                case TitleFamily::LocalRead:
                    if (in.has(TitleShape::kStack)) {
                        s.answer = TitleAnswer::StackBound;      // the frame's body, no title
                    } else if (in.has(TitleShape::kHasEntry)) {
                        // 6.2.2 — a STATIC owner's title is the constant 1: the
                        // entry was armed by the push, not from a callee's flag
                        // or a word bit (kRuntimeOwner), and the scope still
                        // says it owns (kStaticTitle: no earlier move — a move
                        // in either arm of an `if` stays recorded after the
                        // join, a re-assignment restores — and no call-borrow
                        // origin). A second `#x` of a transferred name is
                        // rejected statically, so this is the first move in
                        // flow. Anything else reads the entry's active byte.
                        if (in.has(TitleShape::kStaticTitle) && !in.has(TitleShape::kRuntimeOwner)) {
                            s.answer = TitleAnswer::Owned;
                        } else {
                            s.answer = TitleAnswer::Runtime; s.source = TitleSource::DropEntry;
                        }
                    } else if (in.has(TitleShape::kIsParam)) {
                        if (in.has(TitleShape::kTransferredParam)) {
                            s.answer = TitleAnswer::Owned;           // `#`-formal: the frame's title
                        } else {
                            s.answer = TitleAnswer::Runtime; s.source = TitleSource::TransferWord;
                        }
                    } else if (in.has(TitleShape::kStaticTitle)) {
                        s.answer = TitleAnswer::Owned;            // a static owner without an entry
                    } else {
                        // No entry, not a formal, no static title: a borrow
                        // alias (the Cache.linkAtHead rule) — `#=` records it.
                        s.answer = TitleAnswer::Borrow;
                    }
                    break;
                case TitleFamily::FieldRead:
                case TitleFamily::ElementRead:
                    s.answer = TitleAnswer::Runtime; s.source = TitleSource::Slot;
                    break;
                case TitleFamily::Move:
                case TitleFamily::CallResult:
                case TitleFamily::ClosureCall:
                case TitleFamily::Conditional:
                case TitleFamily::Fresh:
                case TitleFamily::Concat:
                case TitleFamily::Closure:
                    s.answer = in.answer; s.source = in.source;
                    break;
                case TitleFamily::Literal:
                case TitleFamily::ThisRead:
                    s.answer = TitleAnswer::Borrow;
                    break;
                case TitleFamily::Scalar:
                case TitleFamily::Unsupported:
                case TitleFamily::Count:
                    s.answer = in.answer; s.source = in.source;
                    break;
            }
            return s;
    }

    TitleVerdict policy(const TitleShape& s, ConsumerRole role) {
        TitleVerdict v{s.answer, s.source, nullptr, s.label};
        // A scalar has no title in any role.
        if (s.answer == TitleAnswer::Scalar) return v;
        // Spec 5.10 — an array whose slots lend frame locals may not leave
        // the frame: a `#` move of it into a retaining slot, a `#T` argument
        // or a `#` return. (A same-frame `#=` bind carries the record on.)
        auto escapesBorrowedSlots = [&] {
            return s.family == TitleFamily::Move && s.field
                && !s.field->getSlotBorrowedLocals().empty();
        };
        switch (role) {
            case ConsumerRole::StoreString:
            case ConsumerRole::StoreSlot:
                // Spec 5.11 — a `stack` value moved into a retaining slot is
                // rejected; a plain store of it is an ordinary borrow.
                if (s.family == TitleFamily::Move && s.answer == TitleAnswer::StackBound) {
                    v.error = "CAJETA_ERROR_STACK_TRANSFER";
                } else if (escapesBorrowedSlots()) {
                    v.error = "CAJETA_ERROR_ARRAY_SLOT_BORROWS_LOCAL";
                } else if (s.family == TitleFamily::LocalRead
                           && s.has(TitleShape::kIsParam)
                           && s.has(TitleShape::kTransferredParam)) {
                    // A `#`-declared formal HOLDS the frame's title (no entry,
                    // the word bit is not consulted): storing it by bare name
                    // consumes it — the store adopts (uniform-transfer 2.3;
                    // `take(#String s) { this.v = s; }` in
                    // CallArgTempDropTests). The `#` is on the formal.
                    v.answer = TitleAnswer::Owned;
                    v.source = TitleSource::None;
                }
                return v;
            case ConsumerRole::Bind:
            case ConsumerRole::Reassign:
            case ConsumerRole::ArgPlain:
            case ConsumerRole::Arm:
            case ConsumerRole::Count:
                return v;
            case ConsumerRole::ReturnOwned:
                // A `#T` return is a contract: every shape must establish a
                // title (spec §2.3). The frame-bound shapes first — no
                // spelling fixes them: a `stack` value (5.11, as a
                // construction, a moved or bare stack local, or an sret
                // call) and the borrowed receiver.
                if (s.answer == TitleAnswer::StackBound
                        || (s.family == TitleFamily::LocalRead && s.has(TitleShape::kStack))) {
                    v.error = "CAJETA_ERROR_STACK_RETURN_ESCAPES";
                    return v;
                }
                if (escapesBorrowedSlots()) {
                    v.error = "CAJETA_ERROR_ARRAY_SLOT_BORROWS_LOCAL";
                    return v;
                }
                if (s.family == TitleFamily::ThisRead) {
                    v.error = "CAJETA_ERROR_OWNED_RETURN_OF_BORROWED_THIS";
                    return v;
                }
                // `return #x`: the move's own codegen diagnoses a borrow
                // source (MOVE_OF_BORROW, Unit 3); its stashed flag is the
                // answer here.
                if (s.family == TitleFamily::Move) return v;
                if (s.family == TitleFamily::LocalRead) {
                    if (s.has(TitleShape::kBorrowOrigin)) {
                        v.error = "CAJETA_ERROR_OWNED_RETURN_OF_BORROW";
                        return v;
                    }
                    if (s.has(TitleShape::kHasEntry)) {
                        // The entry IS the frame's title. Armed statically it
                        // is a constant; armed at run time (a callee's flag,
                        // a plain formal's word bit) it is read before the
                        // return deactivates it.
                        if (s.has(TitleShape::kRuntimeOwner)) {
                            v.answer = TitleAnswer::Runtime; v.source = TitleSource::DropEntry;
                        } else {
                            v.answer = TitleAnswer::Owned; v.source = TitleSource::None;
                        }
                        return v;
                    }
                    if (s.has(TitleShape::kIsParam)) {
                        // A `#` formal holds the frame's title unconditionally
                        // (Unit 5's store rule). An interface formal has no
                        // entry today and keeps the static mode (finding
                        // 6.1.x). A plain class formal without an entry (a
                        // String) holds nothing to transfer.
                        if (s.has(TitleShape::kTransferredParam) || s.has(TitleShape::kInterface)) {
                            v.answer = TitleAnswer::Owned; v.source = TitleSource::None;
                            return v;
                        }
                        v.error = "CAJETA_ERROR_BORROW_PARAM_ESCAPES";
                        return v;
                    }
                    // No entry, not a formal: the local lends someone else's
                    // value (a literal bind, an alias of another local).
                    v.error = "CAJETA_ERROR_OWNED_RETURN_OF_BORROW";
                    return v;
                }
                // A String literal's static wrapper is adopted, not
                // borrowed: `__cajeta_string_drop` is a no-op on it (the
                // live-set claim fails), so the caller's drop entry costs
                // nothing and frees nothing — the enum `toName()` idiom
                // (`return "error";` under `#String`). Measured 2026-09-08:
                // the stdlib's every enum returns one.
                if (s.family == TitleFamily::Literal && s.has(TitleShape::kString)) {
                    v.answer = TitleAnswer::Owned; v.source = TitleSource::None;
                    return v;
                }
                if (s.answer == TitleAnswer::Borrow) {
                    v.error = "CAJETA_ERROR_OWNED_RETURN_OF_BORROW";
                }
                return v;
            case ConsumerRole::ReturnPlain:
                // A plain return hands out no fresh value (nobody registers a
                // drop for it), no owned local (dropped before the ret) and
                // no class-typed `stack` value (reclaimed at the ret); a
                // formal forwards its entry flag; a call's flag rides
                // through; a borrow is a borrow.
                if (s.family == TitleFamily::Fresh) {
                    if (s.answer == TitleAnswer::StackBound) {
                        v.error = "CAJETA_ERROR_STACK_RETURN_ESCAPES";
                    } else if (s.answer == TitleAnswer::Owned) {
                        v.error = "CAJETA_ERROR_FRESH_RETURN_NEEDS_TRANSFER";
                    }
                    return v;
                }
                if (s.family == TitleFamily::LocalRead) {
                    if (s.has(TitleShape::kIsParam)) {
                        // The pass-through: a plain formal's entry carries the
                        // caller's mode out. A `#` formal or an entry-less
                        // formal keeps the static borrow (unchecked today —
                        // finding 6.1.x).
                        if (s.has(TitleShape::kHasEntry)) {
                            v.answer = TitleAnswer::Runtime; v.source = TitleSource::DropEntry;
                        }
                        return v;
                    }
                    if (s.has(TitleShape::kStack)) {
                        v.error = "CAJETA_ERROR_STACK_RETURN_ESCAPES";
                        return v;
                    }
                    // Keys on the ENTRY, not the title (8.2.10): a local that
                    // merely holds a call's borrow is indistinguishable here
                    // and is rejected too — the accepted cost. Value locals
                    // return by copy, closures have their own protocol, and
                    // an interface local's entry is its kind word.
                    if (s.has(TitleShape::kHasEntry) && !s.has(TitleShape::kValue)
                            && !s.has(TitleShape::kFunction) && !s.has(TitleShape::kInterface)) {
                        v.error = "CAJETA_ERROR_FRESH_RETURN_NEEDS_TRANSFER";
                    }
                    return v;
                }
                return v;
            case ConsumerRole::ArgOwned:
                // A `#T` formal is a promise the argument must keep: a proven
                // borrow is rejected (spec 5.8); an owned or runtime-decided
                // value passes and its bit rides the transfer word.
                if (escapesBorrowedSlots()) {
                    v.error = "CAJETA_ERROR_ARRAY_SLOT_BORROWS_LOCAL";
                    return v;
                }
                switch (s.family) {
                    case TitleFamily::FieldRead:
                    case TitleFamily::ElementRead:
                    case TitleFamily::Literal:
                    case TitleFamily::ThisRead:
                        v.error = "CAJETA_ERROR_TRANSFER_REQUIRED";
                        return v;
                    case TitleFamily::CallResult:
                    case TitleFamily::ClosureCall:
                        if (s.answer == TitleAnswer::Borrow) v.error = "CAJETA_ERROR_TRANSFER_REQUIRED";
                        return v;
                    case TitleFamily::LocalRead:
                        if (s.has(TitleShape::kIsParam) && !s.has(TitleShape::kTransferredParam)) {
                            v.error = "CAJETA_ERROR_TRANSFER_REQUIRED";   // a borrowed parameter
                            return v;
                        }
                        if (!s.has(TitleShape::kHasEntry) && !s.has(TitleShape::kIsParam)
                                && !s.has(TitleShape::kArena)) {
                            v.error = "CAJETA_ERROR_TRANSFER_REQUIRED";   // an entry-less local (5.8)
                            return v;
                        }
                        if (s.has(TitleShape::kHasEntry)) {
                            // An owned local must be surrendered with `#k`.
                            v.error = "CAJETA_ERROR_TRANSFER_REQUIRED";
                        }
                        return v;
                    case TitleFamily::Fresh:
                    case TitleFamily::Concat:
                    case TitleFamily::Move:
                    case TitleFamily::Conditional:
                    case TitleFamily::Closure:
                    case TitleFamily::Scalar:
                    case TitleFamily::Unsupported:
                    case TitleFamily::Count:
                        return v;
                }
                return v;
        }
        return v;
    }

    namespace {
        // The entry's active byte (offset 24: obj, drop_fn, prev, active),
        // read inline — one GEP, one load, one zext.
        llvm::Value* entryActiveFlag(llvm::Value* entry, const CajetaModulePtr& module) {
            auto* builder = module->getBuilder();
            auto& ctx = *module->getLlvmContext();
            llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
            llvm::Value* activePtr = builder->CreateInBoundsGEP(
                llvm::Type::getInt8Ty(ctx), entry,
                llvm::ConstantInt::get(i64, 24), "title.active.ptr");
            llvm::Value* active = builder->CreateLoad(
                llvm::Type::getInt8Ty(ctx), activePtr, "title.active");
            return builder->CreateZExt(active, i64, "title.flag");
        }
    }  // namespace

    namespace {
        void throwVerdict(const TitleShape& s, const TitleVerdict& v, const ExpressionPtr& e,
                          const CajetaModulePtr& module, const char* where) {
            std::string what = v.error;
            std::string msg;
            if (what == "CAJETA_ERROR_STACK_TRANSFER") {
                msg = std::string("`stack` value transferred into ") + where
                    + ": a stack instance dies with its frame, so a `#` move into a "
                      "field or slot that outlives it would dangle. Construct it "
                      "with `heap` to transfer it, or return it by value (a plain "
                      "`T` return of `stack T(...)` lands in the caller's frame). "
                      "See docs/specification/lang/MemoryModel.md § Placement.";
            } else if (what == "CAJETA_ERROR_ARRAY_SLOT_BORROWS_LOCAL") {
                const char* arr = s.field ? s.field->getName().c_str() : "the array";
                std::string slotText;
                std::string localsFix;
                if (s.field) {
                    for (auto& p : s.field->getSlotBorrowedLocals()) {
                        if (slotText.empty()) {
                            slotText = (p.first >= 0 ? "slot " + std::to_string(p.first)
                                                     : std::string("a slot"))
                                + " borrows local `" + p.second + "`";
                        }
                        if (!localsFix.empty()) localsFix += ", ";
                        localsFix += "#" + p.second;
                    }
                }
                msg = std::string("array `") + arr + "` leaves the frame through "
                    + where + ", but " + slotText + ", which dies with this frame: "
                      "the slot would dangle. A bare name in an array literal or "
                      "element store LENDS (like `list.add(x)`); write `"
                    + localsFix + "` to move the value into the array, or store a "
                      "copy. See specs/ownership-title-classifier-spec.md 5.10.";
            } else {
                msg = std::string(v.error) + " at " + where + " (" + v.label + ")";
            }
            throw Exception(msg, what, module->getSourcePath(),
                            (int) e->getSourceLine(), (int) e->getSourceColumn());
        }
    }  // namespace

    void rejectEscape(const ExpressionPtr& e, ConsumerRole role,
                      const CajetaModulePtr& module, const char* where) {
        if (!e) return;
        TitleShape s = classify(e, module);
        // Only the escape rule (spec 5.10) is asked here: the return and
        // argument sites keep their own title checks until Units 6 and 7
        // migrate them, and a plain-return `#x` / `return #= x` must not be
        // judged by the `#T`-return contract.
        // A `#r` argument may arrive as the bare name with the call's
        // transferred flag set, so a LocalRead counts here too: the array is
        // leaving the frame whichever way the `#` was carried.
        if ((s.family == TitleFamily::Move || s.family == TitleFamily::LocalRead)
                && s.field && !s.field->getSlotBorrowedLocals().empty()) {
            TitleVerdict v{s.answer, s.source, "CAJETA_ERROR_ARRAY_SLOT_BORROWS_LOCAL", s.label};
            throwVerdict(s, v, e, module, where);
        }
        (void) role;
    }

    llvm::Value* storeTitleFlag(const ExpressionPtr& e, ConsumerRole role,
                                const CajetaModulePtr& module, const char* where) {
        if (!e) return nullptr;
        return storeTitleFlagOf(classify(e, module), role, e, module, where);
    }

    llvm::Value* verdictFlag(const TitleShape& s, const TitleVerdict& v,
                             const CajetaModulePtr& module) {
        if (v.error || v.answer == TitleAnswer::Scalar) return nullptr;
        TitleShape r = s;
        r.answer = v.answer;
        r.source = v.source;
        return titleFlag(r, module);
    }

    llvm::Value* storeTitleFlagOf(const TitleShape& s, ConsumerRole role,
                                  const ExpressionPtr& e, const CajetaModulePtr& module,
                                  const char* where) {
        TitleVerdict v = policy(s, role);
        if (v.error) throwVerdict(s, v, e, module, where);
        switch (v.answer) {
            case TitleAnswer::Owned:
                // The VERDICT's answer, not the shape's: a policy row may
                // promote (a `#` formal stored by bare name).
                return llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(*module->getLlvmContext()), 1);
            case TitleAnswer::Runtime:
                return titleFlag(s, module);
            case TitleAnswer::Borrow:
            case TitleAnswer::StackBound:
            case TitleAnswer::Scalar:
                return nullptr;
        }
        return nullptr;
    }

    llvm::Value* titleFlag(const TitleShape& s, const CajetaModulePtr& module) {
        auto* builder = module->getBuilder();
        auto& ctx = *module->getLlvmContext();
        llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
        switch (s.answer) {
            case TitleAnswer::Borrow:
            case TitleAnswer::StackBound:
            case TitleAnswer::Scalar:
                return llvm::ConstantInt::get(i64, 0);
            case TitleAnswer::Owned:
                return llvm::ConstantInt::get(i64, 1);
            case TitleAnswer::Runtime:
                break;
        }
        // A move already read its source's flag BEFORE deactivating it (the
        // entry now says 0); the stashed value is the truth, never a re-read.
        if (s.family == TitleFamily::Move && s.leaf && s.leaf->kind() == ExprKind::Move) {
            if (llvm::Value* mf = std::static_pointer_cast<MoveExpression>(s.leaf)->getRuntimeTitleFlag()) {
                return mf;
            }
            return llvm::ConstantInt::get(i64, 1);
        }
        llvm::Function* fn = builder->GetInsertBlock() ? builder->GetInsertBlock()->getParent() : nullptr;
        if (s.leaf && fn && s.leaf->titleFlagCacheFor(fn)) {
            return s.leaf->titleFlagCacheFor(fn);
        }
        llvm::Value* v = nullptr;
        switch (s.source) {
            case TitleSource::DropEntry: {
                if (!s.field || !s.field->getDropEntry()) break;
                v = entryActiveFlag(s.field->getDropEntry(), module);
                break;
            }
            case TitleSource::TransferWord: {
                auto m = module->getCurrentMethod();
                llvm::Value* word = m ? m->getTransferWordArg() : nullptr;
                if (!word || s.paramIndex < 0) break;
                v = builder->CreateAnd(
                    builder->CreateLShr(word, llvm::ConstantInt::get(i64, (uint64_t) s.paramIndex)),
                    llvm::ConstantInt::get(i64, 1), "title.word.bit");
                break;
            }
            case TitleSource::ReturnFlag: {
                if (llvm::Function* gf = module->getRuntimeFunction("__cajeta_return_flag_get")) {
                    v = builder->CreateCall(gf, {}, "title.ret.flag");
                }
                break;
            }
            case TitleSource::ArmPhi: {
                v = conditionalTitleFlag(s.leaf);   // either conditional kind
                break;
            }
            case TitleSource::Slot: {
                if (s.leaf && s.leaf->kind() == ExprKind::Move) {
                    v = std::static_pointer_cast<MoveExpression>(s.leaf)->getRuntimeTitleFlag();
                }
                break;
            }
            case TitleSource::None:
                break;
        }
        if (v && s.leaf && fn) s.leaf->setTitleFlagCache(v, fn);
        return v;
    }

    bool TitleShapeAudit::enabled() { return g_auditEnabled; }
    void TitleShapeAudit::setEnabled(bool on) { g_auditEnabled = on; }
    void TitleShapeAudit::record(TitleShapeRecord rec) { g_records.push_back(std::move(rec)); }
    const std::vector<TitleShapeRecord>& TitleShapeAudit::records() { return g_records; }
    void TitleShapeAudit::clear() { g_records.clear(); }

    void observeTitle(const ExpressionPtr& e, const CajetaModulePtr& module, ConsumerRole role) {
        if (!g_auditEnabled || !e) return;
        TitleShape s = classify(e, module);
        TitleVerdict v = policy(s, role);
        TitleShapeRecord r;
        r.file = module->getSourcePath();
        if (auto hm = module->getCurrentMethod()) {
            r.holder = (hm->getParent() ? hm->getParent()->toCanonical() + "." : std::string())
                + hm->getName();
        }
        r.line = (int) e->getSourceLine();
        r.kind = e->kind();
        r.family = s.family;
        r.answer = v.answer;
        r.source = v.source;
        r.role = role;
        r.flags = s.flags;
        g_records.push_back(std::move(r));
    }

    const char* toString(TitleAnswer a) noexcept {
        switch (a) {
            case TitleAnswer::Borrow: return "Borrow";
            case TitleAnswer::Owned: return "Owned";
            case TitleAnswer::StackBound: return "StackBound";
            case TitleAnswer::Runtime: return "Runtime";
            case TitleAnswer::Scalar: return "Scalar";
        }
        return "?";
    }
    const char* toString(TitleSource s) noexcept {
        switch (s) {
            case TitleSource::None: return "None";
            case TitleSource::DropEntry: return "DropEntry";
            case TitleSource::TransferWord: return "TransferWord";
            case TitleSource::ReturnFlag: return "ReturnFlag";
            case TitleSource::ArmPhi: return "ArmPhi";
            case TitleSource::Slot: return "Slot";
        }
        return "?";
    }
    const char* toString(TitleFamily f) noexcept {
        switch (f) {
            case TitleFamily::Literal: return "Literal";
            case TitleFamily::LocalRead: return "LocalRead";
            case TitleFamily::ThisRead: return "ThisRead";
            case TitleFamily::FieldRead: return "FieldRead";
            case TitleFamily::ElementRead: return "ElementRead";
            case TitleFamily::Fresh: return "Fresh";
            case TitleFamily::Concat: return "Concat";
            case TitleFamily::Move: return "Move";
            case TitleFamily::CallResult: return "CallResult";
            case TitleFamily::ClosureCall: return "ClosureCall";
            case TitleFamily::Conditional: return "Conditional";
            case TitleFamily::Closure: return "Closure";
            case TitleFamily::Scalar: return "Scalar";
            case TitleFamily::Unsupported: return "Unsupported";
            case TitleFamily::Count: return "Count";
        }
        return "?";
    }
    const char* toString(ConsumerRole r) noexcept {
        switch (r) {
            case ConsumerRole::Bind: return "Bind";
            case ConsumerRole::StoreString: return "StoreString";
            case ConsumerRole::StoreSlot: return "StoreSlot";
            case ConsumerRole::Reassign: return "Reassign";
            case ConsumerRole::ReturnOwned: return "ReturnOwned";
            case ConsumerRole::ReturnPlain: return "ReturnPlain";
            case ConsumerRole::ArgPlain: return "ArgPlain";
            case ConsumerRole::ArgOwned: return "ArgOwned";
            case ConsumerRole::Arm: return "Arm";
            case ConsumerRole::Count: return "Count";
        }
        return "?";
    }
    const char* toString(ExprKind k) noexcept {
        switch (k) {
            case ExprKind::Unsupported: return "Unsupported";
            case ExprKind::Primary: return "Primary";
            case ExprKind::Literal: return "Literal";
            case ExprKind::ClassLiteral: return "ClassLiteral";
            case ExprKind::This: return "This";
            case ExprKind::Super: return "Super";
            case ExprKind::TextLiteral: return "TextLiteral";
            case ExprKind::IntegerLiteral: return "IntegerLiteral";
            case ExprKind::FloatLiteral: return "FloatLiteral";
            case ExprKind::Identifier: return "Identifier";
            case ExprKind::Dot: return "Dot";
            case ExprKind::ArrayIndex: return "ArrayIndex";
            case ExprKind::ArraySlice: return "ArraySlice";
            case ExprKind::ArrayLiteral: return "ArrayLiteral";
            case ExprKind::MapLiteral: return "MapLiteral";
            case ExprKind::Aggregate: return "Aggregate";
            case ExprKind::New: return "New";
            case ExprKind::Cast: return "Cast";
            case ExprKind::Postfix: return "Postfix";
            case ExprKind::Prefix: return "Prefix";
            case ExprKind::BinaryOp: return "BinaryOp";
            case ExprKind::BooleanSwitch: return "BooleanSwitch";
            case ExprKind::InstanceOf: return "InstanceOf";
            case ExprKind::MethodCall: return "MethodCall";
            case ExprKind::Call: return "Call";
            case ExprKind::MethodReference: return "MethodReference";
            case ExprKind::Move: return "Move";
            case ExprKind::Await: return "Await";
            case ExprKind::Spawn: return "Spawn";
            case ExprKind::Detach: return "Detach";
            case ExprKind::Switch: return "Switch";
            case ExprKind::Lambda: return "Lambda";
            case ExprKind::Count: return "Count";
        }
        return "?";
    }

}  // namespace cajeta::ownership
