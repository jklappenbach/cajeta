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
#include "cajeta/field/Field.h"
#include "cajeta/field/ParameterField.h"
#include "cajeta/method/Method.h"
#include "cajeta/type/CajetaArray.h"
#include "cajeta/type/CajetaClass.h"
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

        inline uint16_t typeFlags(const CajetaTypePtr& t) {
            uint16_t f = 0;
            if (!t) return f;
            if (isLangString(t)) f |= TitleShape::kString;
            if (std::dynamic_pointer_cast<CajetaArray>(t)) f |= TitleShape::kArray;
            if (std::dynamic_pointer_cast<CajetaView>(t)) f |= TitleShape::kView;
            if (auto cls = std::dynamic_pointer_cast<CajetaClass>(t)) {
                if (cls->isInterface()) f |= TitleShape::kInterface;
                if (cls->isValueType()) f |= TitleShape::kValue;
            }
            return f;
        }

        inline TitleShape make(TitleFamily fam, TitleAnswer ans, TitleSource src,
                               const ExpressionPtr& leaf, uint16_t flags = 0) {
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
                                (uint16_t) ((a.flags | b.flags)
                                            & (TitleShape::kString | TitleShape::kArray
                                               | TitleShape::kInterface | TitleShape::kValue
                                               | TitleShape::kView)));
            if (a.answer == b.answer && a.answer != TitleAnswer::Runtime) {
                s.answer = a.answer;
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
            return s;
        }

        TitleShape callResult(const ExpressionPtr& leaf, const CajetaModulePtr& module) {
            auto mce = std::static_pointer_cast<MethodCallExpression>(leaf);
            uint16_t flags = typeFlags(leaf->getResolvedType());
            MethodPtr rm = MethodCallExpression::resolveArgCalleeShallow(mce, module);
            if (!rm) {
                // Unresolvable before codegen: the callee's own flag decides
                // at run time (spec §2.1, CallResult).
                return make(TitleFamily::CallResult, TitleAnswer::Runtime,
                            TitleSource::ReturnFlag, leaf, flags);
            }
            flags |= typeFlags(rm->getReturnType());
            if (isOwnershipLessScalar(rm->getReturnType())) return scalar(leaf);
            if (rm->isReturnsView()) {
                return make(TitleFamily::CallResult, TitleAnswer::Borrow,
                            TitleSource::None, leaf, (uint16_t) (flags | TitleShape::kView));
            }
            if (rm->isReturnsOwnership()) {
                return make(TitleFamily::CallResult, TitleAnswer::Owned,
                            TitleSource::None, leaf, flags);
            }
            // A plain return is NOT statically a borrow (ownership §2.1): the
            // callee leaves its bit in the TLS. A callee that emits no flag (a
            // native, an intrinsic) is its declared stance: plain → Borrow.
            if (rm->emitsReturnFlag() && rm->returnsClassPointer()) {
                return make(TitleFamily::CallResult, TitleAnswer::Runtime,
                            TitleSource::ReturnFlag, leaf, flags);
            }
            return make(TitleFamily::CallResult, TitleAnswer::Borrow,
                        TitleSource::None, leaf, flags);
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
                uint16_t f = typeFlags(e->getResolvedType()) | TitleShape::kArray;
                if (al->isStackAlloc()) f |= TitleShape::kStack;
                if (al->isArenaEligible()) f |= TitleShape::kArena;
                bool bound = al->isStackAlloc() || al->isArenaEligible();
                return make(TitleFamily::Fresh, bound ? TitleAnswer::StackBound : TitleAnswer::Owned,
                            TitleSource::None, e, f);
            }
            case ExprKind::MapLiteral:
                return make(TitleFamily::Fresh, TitleAnswer::Owned, TitleSource::None, e,
                            typeFlags(e->getResolvedType()));
            case ExprKind::Aggregate: {
                auto ag = std::static_pointer_cast<AggregateInitializerExpression>(e);
                uint16_t f = typeFlags(e->getResolvedType());
                if (ag->getStackAlloc()) {
                    return make(TitleFamily::Fresh, TitleAnswer::StackBound, TitleSource::None, e,
                                (uint16_t) (f | TitleShape::kStack));
                }
                return make(TitleFamily::Fresh, TitleAnswer::Owned, TitleSource::None, e, f);
            }
            case ExprKind::New: {
                auto ne = std::static_pointer_cast<NewExpression>(e);
                uint16_t f = typeFlags(e->getResolvedType());
                if (ne->getStackAlloc()) {
                    return make(TitleFamily::Fresh, TitleAnswer::StackBound, TitleSource::None, e,
                                (uint16_t) (f | TitleShape::kStack));
                }
                if (ne->getSharedAlloc()) {
                    return make(TitleFamily::Fresh, TitleAnswer::Borrow, TitleSource::None, e,
                                (uint16_t) (f | TitleShape::kShared));
                }
                return make(TitleFamily::Fresh, TitleAnswer::Owned, TitleSource::None, e, f);
            }
            case ExprKind::BinaryOp: {
                auto bo = std::static_pointer_cast<BinaryOpExpression>(e);
                BinaryOp op = bo->getBinaryOp();
                if (op == BINARY_OP_ADD && isLangString(bo->getResolvedType())) {
                    uint16_t f = TitleShape::kString;
                    if (bo->isArenaEligible()) {
                        return make(TitleFamily::Concat, TitleAnswer::StackBound, TitleSource::None,
                                    e, (uint16_t) (f | TitleShape::kArena));
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
            case ExprKind::Call:
                return make(TitleFamily::ClosureCall, TitleAnswer::Runtime, TitleSource::ReturnFlag,
                            e, typeFlags(e->getResolvedType()));
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
            TitleShape s = make(TitleFamily::Move, TitleAnswer::Owned, TitleSource::None, leaf);
            if (mv->isSharpStore()) s.flags |= TitleShape::kSharpStore;
            if (!inner) return s;
            TitleShape in = classify(inner, module);
            s.flags |= in.flags;
            s.field = in.field;
            s.paramIndex = in.paramIndex;
            switch (in.family) {
                case TitleFamily::LocalRead:
                    if (in.has(TitleShape::kHasEntry)) {
                        s.answer = TitleAnswer::Runtime; s.source = TitleSource::DropEntry;
                    } else if (in.has(TitleShape::kIsParam)) {
                        if (in.has(TitleShape::kTransferredParam)) {
                            s.answer = TitleAnswer::Owned;           // `#`-formal: the frame's title
                        } else {
                            s.answer = TitleAnswer::Runtime; s.source = TitleSource::TransferWord;
                        }
                    } else {
                        // No entry, not a formal: a borrow alias (the
                        // Cache.linkAtHead rule) — `#=` records it as such.
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
    }  // namespace

    TitleVerdict policy(const TitleShape& s, ConsumerRole role) {
        TitleVerdict v{s.answer, s.source, nullptr, s.label};
        // A scalar has no title in any role.
        if (s.answer == TitleAnswer::Scalar) return v;
        switch (role) {
            case ConsumerRole::Bind:
            case ConsumerRole::StoreString:
            case ConsumerRole::StoreSlot:
            case ConsumerRole::Reassign:
            case ConsumerRole::ArgPlain:
            case ConsumerRole::Arm:
            case ConsumerRole::Count:
                return v;
            case ConsumerRole::ReturnOwned:
                // A `#T` return is a contract: every shape must establish a
                // title (spec §2.3, OWNED_RETURN_OF_BORROW / BORROWED_THIS /
                // STACK_RETURN_ESCAPES). A bare local that OWNS forwards its
                // entry flag (the return deactivates it); a formal forwards
                // its word bit.
                if (s.answer == TitleAnswer::StackBound) {
                    v.error = "CAJETA_ERROR_STACK_RETURN_ESCAPES";
                    return v;
                }
                if (s.family == TitleFamily::ThisRead) {
                    v.error = "CAJETA_ERROR_OWNED_RETURN_OF_BORROWED_THIS";
                    return v;
                }
                if (s.family == TitleFamily::LocalRead) {
                    if (s.has(TitleShape::kBorrowOrigin)) {
                        v.error = "CAJETA_ERROR_OWNED_RETURN_OF_BORROW";
                        return v;
                    }
                    if (s.has(TitleShape::kHasEntry)) {
                        v.answer = TitleAnswer::Runtime; v.source = TitleSource::DropEntry;
                        return v;
                    }
                    if (s.has(TitleShape::kIsParam)) {
                        v.answer = TitleAnswer::Runtime; v.source = TitleSource::TransferWord;
                        return v;
                    }
                    v.error = "CAJETA_ERROR_OWNED_RETURN_OF_BORROW";
                    return v;
                }
                if (s.answer == TitleAnswer::Borrow) {
                    v.error = "CAJETA_ERROR_OWNED_RETURN_OF_BORROW";
                }
                return v;
            case ConsumerRole::ReturnPlain:
                // A plain return may not hand out a fresh value (nobody
                // registers a drop for it) nor an owned local (dropped before
                // the ret); a call's flag rides through; a borrow is a borrow.
                if (s.family == TitleFamily::Fresh && s.answer == TitleAnswer::Owned) {
                    v.error = "CAJETA_ERROR_FRESH_RETURN_NEEDS_TRANSFER";
                    return v;
                }
                if (s.family == TitleFamily::LocalRead && s.has(TitleShape::kHasEntry)
                        && !s.has(TitleShape::kIsParam)) {
                    v.error = "CAJETA_ERROR_FRESH_RETURN_NEEDS_TRANSFER";
                    return v;
                }
                if (s.family == TitleFamily::LocalRead && s.has(TitleShape::kIsParam)) {
                    v.answer = TitleAnswer::Runtime; v.source = TitleSource::TransferWord;
                    return v;
                }
                return v;
            case ConsumerRole::ArgOwned:
                // A `#T` formal is a promise the argument must keep: a proven
                // borrow is rejected (spec 5.8); an owned or runtime-decided
                // value passes and its bit rides the transfer word.
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
        llvm::Function* fn = builder->GetInsertBlock() ? builder->GetInsertBlock()->getParent() : nullptr;
        if (s.leaf && fn && s.leaf->titleFlagCacheFor(fn)) {
            return s.leaf->titleFlagCacheFor(fn);
        }
        llvm::Value* v = nullptr;
        switch (s.source) {
            case TitleSource::DropEntry: {
                // The entry's active byte (offset 24: obj, drop_fn, prev,
                // active), read inline — one load, one zext.
                if (!s.field || !s.field->getDropEntry()) break;
                llvm::Value* activePtr = builder->CreateInBoundsGEP(
                    llvm::Type::getInt8Ty(ctx), s.field->getDropEntry(),
                    llvm::ConstantInt::get(i64, 24), "title.active.ptr");
                llvm::Value* active = builder->CreateLoad(
                    llvm::Type::getInt8Ty(ctx), activePtr, "title.active");
                v = builder->CreateZExt(active, i64, "title.flag");
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
                if (!s.leaf) break;
                if (s.leaf->kind() == ExprKind::BooleanSwitch) {
                    v = std::static_pointer_cast<BooleanSwitchExpression>(s.leaf)->getRuntimeTitleFlag();
                }
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
