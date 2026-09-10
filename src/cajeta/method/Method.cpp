//
// Created by James Klappenbach on 2/19/22.
//

#include "../error/Diagnostics.h"
#include "Method.h"
#include <atomic>
#include <functional>
#include <unordered_set>
#include "../type/CajetaClass.h"
#include "../field/StackField.h"
#include "../type/CajetaView.h"
#include "../type/CajetaArray.h"
#include "../type/CajetaFunctionType.h"
#include "../compile/CajetaModule.h"
#include "../prof/ProfileCodegen.h"
#include "../compile/Compiler.h"
#include "../compile/ExcFrameSetjmp.h"
#include "../compile/ScriptUnitSynthesis.h"
#include "../error/VariableAssignmentException.h"
#include "../error/Exception.h"
#include "../asn/DefaultBlock.h"
#include "../asn/Statement.h"
#include "../asn/expression/Expression.h"
#include "../asn/expression/MethodCallExpression.h"
#include "../asn/expression/CreatorRest.h"
#include "../asn/expression/NewExpression.h"
#include "../asn/expression/AggregateInitializerExpression.h"
#include "../asn/expression/BinaryOpExpression.h"
#include "../asn/expression/Identifier.h"
#include "../asn/expression/DotExpression.h"
#include "../asn/expression/LiteralExpression.h"
#include "../asn/LocalVariableDeclaration.h"
#include "../type/FormalParameter.h"
#include "../field/ParameterField.h"
#include "../field/BoundClosureField.h"
#include "cajeta/dbg/DebugCodegen.h"
#include "cajeta/dbg/LineInfoCodegen.h"
#include "../xpu/core/KernelArgTrait.h"
#include "../xpu/core/XpuAttributes.h"

using namespace std;

#define ERROR_CAUSE_ASSIGNMENT_FINAL        "the field is declared final"
#define ERROR_CAUSE_VARIABLE_NOT_FOUND      "the field was not declared"
#define ERROR_CAUSE_VARIABLE_DUPLICATE      "the field name already exists"
#define ERROR_ID_ASSIGNMENT_FINAL           "CAJETA_ERROR_FINAL_ASSIGNMENT"
#define ERROR_ID_VARIABLE_NOT_FOUND         "CAJETA_ERROR_VARIABLE_NOT_FOUND"
#define ERROR_ID_VARIABLE_DUPLICATE         "CAJETA_ERROR_VARIABLE_DUPLICATE"

namespace cajeta {
    thread_local map<string, MethodPtr> Method::archive;

    bool Method::isFrozen() const {
        return parent && parent->isFrozen();
    }

    std::unordered_map<const Method*, MethodLlvmBindings>& Method::frozenMethodBindings() {
        static thread_local std::unordered_map<const Method*, MethodLlvmBindings> tbl;
        return tbl;
    }

    llvm::Function*& Method::llvmFunctionRef() {
        return isFrozen() ? frozenMethodBindings()[this].llvmFunction : llvmFunction;
    }
    llvm::FunctionType*& Method::llvmFunctionTypeRef() {
        return isFrozen() ? frozenMethodBindings()[this].llvmFunctionType : llvmFunctionType;
    }
    llvm::Function*& Method::llvmOriginalFunctionRef() {
        return isFrozen() ? frozenMethodBindings()[this].llvmOriginalFunction : llvmOriginalFunction;
    }

    // True when the body lexically contains a spawn/detach/scope and therefore
    // needs the per-method scope frame. Containers with no sub-node accessors
    // (try/switch/synchronized/throw/yield) count conservatively as spawn sites.
    static bool astHasSpawnSite(const AbstractSyntaxNodePtr& node);

    static bool anyHasSpawnSite(const vector<AbstractSyntaxNodePtr>& v) {
        for (auto& c : v) if (astHasSpawnSite(c)) return true;
        return false;
    }

    static bool astHasSpawnSite(const AbstractSyntaxNodePtr& node) {
        if (!node) return false;
        if (dynamic_pointer_cast<LambdaExpression>(node)) return false; // own method
        if (dynamic_pointer_cast<SpawnExpression>(node)) return true;
        if (dynamic_pointer_cast<DetachExpression>(node)) return true;
        if (dynamic_pointer_cast<ScopeStatement>(node)) return true;
        if (dynamic_pointer_cast<TryStatement>(node)) return true;
        if (dynamic_pointer_cast<SwitchStatement>(node)) return true;
        if (dynamic_pointer_cast<SynchronizedStatement>(node)) return true;
        if (dynamic_pointer_cast<ThrowStatement>(node)) return true;
        if (dynamic_pointer_cast<YieldStatement>(node)) return true;
        if (anyHasSpawnSite(node->getChildren())) return true;
        // Control-flow containers: recurse their typed sub-node fields.
        if (auto s = dynamic_pointer_cast<IfStatement>(node))
            return astHasSpawnSite(s->getCondition()) || astHasSpawnSite(s->getThenBranch())
                || astHasSpawnSite(s->getElseBranch());
        if (auto s = dynamic_pointer_cast<WhileStatement>(node))
            return astHasSpawnSite(s->getCondition()) || astHasSpawnSite(s->getBody());
        if (auto s = dynamic_pointer_cast<DoStatement>(node))
            return astHasSpawnSite(s->getBody()) || astHasSpawnSite(s->getCondition());
        if (auto s = dynamic_pointer_cast<ForStatement>(node)) {
            if (astHasSpawnSite(s->getInit()) || astHasSpawnSite(s->getCondition())
                    || astHasSpawnSite(s->getBody())) return true;
            for (auto& u : s->getUpdate()) if (astHasSpawnSite(u)) return true;
            return false;
        }
        if (auto s = dynamic_pointer_cast<EnhancedForStatement>(node))
            return astHasSpawnSite(s->getIterableExpr()) || astHasSpawnSite(s->getBody());
        if (auto s = dynamic_pointer_cast<ReturnStatement>(node))
            return astHasSpawnSite(s->getExpression());
        if (auto s = dynamic_pointer_cast<ExpressionStatement>(node))
            return astHasSpawnSite(s->getExpression());
        if (auto s = dynamic_pointer_cast<LabelStatement>(node))
            return astHasSpawnSite(s->getBlock());
        return false;
    }

    bool Method::bodyNeedsScopeFrame() {
        return astHasSpawnSite(block);
    }

    /** Builds a method from its parsed signature and body, on class `parent`. */
    Method::Method(CajetaModulePtr module,
        string& name,
        CajetaTypePtr returnType,
        vector<FormalParameterPtr> parameterList,
        BlockPtr block,
        CajetaClassPtr parent) {
        this->module = module;
        this->parent = parent;
        // Adopt the parent's emit target at construction, so IR emitted during the
        // template body walk already targets the user module (test-reuse only).
        if (parent && parent->getEmitModule() != parent->getModule()) {
            this->emitModule = parent->getEmitModule();
        }
        this->name = name;
        this->returnType = returnType;
        this->parameterList = parameterList;
        this->block = block;
        // A template instantiation's source ctor is named after the unparameterized
        // template, so fall back to the origin's type name to recognize it.
        constructor = parent->getQName()->getTypeName() == name;
        if (!constructor && parent->getTemplateOrigin()) {
            constructor = parent->getTemplateOrigin()->getQName()->getTypeName() == name;
        }
        llvmBasicBlock = nullptr;
    }

    /** Builds a method with an empty default body — the no-body declaration shape. */
    Method::Method(CajetaModulePtr module,
        string name,
        CajetaTypePtr returnType,
        CajetaClassPtr parent) {
        this->module = module;
        this->parent = parent;
        if (parent && parent->getEmitModule() != parent->getModule()) {
            this->emitModule = parent->getEmitModule();
        }
        this->name = name;
        this->returnType = returnType;
        constructor = parent->getQName()->getTypeName() == name;
        block = make_shared<DefaultBlock>();
        llvmBasicBlock = nullptr;
    }

    map<string, MethodPtr>& Method::getArchive() { return archive; }

    void Method::setBlock(BlockPtr block) {
        this->block = block;
    }

    // --- Value-return determination ----------------------------------------
    bool Method::isValueShapeReturnType(const CajetaTypePtr& t) {
        auto klass = dynamic_pointer_cast<CajetaClass>(t);
        if (!klass || klass->isInterface()) return false;
        QualifiedNamePtr qn = klass->getQName();
        if (!qn) return false;
        // The type name carries the arguments ("Optional<int32>"): match the base.
        const std::string& tn = qn->getTypeName();
        bool isOptionalName = tn == "Optional" || tn.rfind("Optional<", 0) == 0;
        return isOptionalName && qn->getPackageName() == "cajeta.lang";
    }

    bool Method::exprIsStackConstruction(const ExpressionPtr& e) {
        if (!e) return false;
        if (auto ne = dynamic_pointer_cast<NewExpression>(e)) {
            return ne->getStackAlloc();
        }
        if (auto ai = dynamic_pointer_cast<AggregateInitializerExpression>(e)) {
            return ai->getStackAlloc();
        }
        return false;
    }

    // True only when `e` provably reads the receiver's own interior (`this.f`, or
    // an index into it). A plain return is NOT statically a borrow — the return
    // flag is runtime state — so every other shape must be allowed, not guessed at.
    bool Method::exprIsInteriorRead(const ExpressionPtr& e) {
        if (!e) return false;
        if (auto ix = dynamic_pointer_cast<ArrayIndexExpression>(e)) {
            auto& ch = ix->getChildren();
            return !ch.empty()
                && exprIsInteriorRead(dynamic_pointer_cast<Expression>(ch[0]));
        }
        // `this` is a ThisExpression node, not an identifier whose text is "this";
        // testing only the identifier form silently disables the whole check.
        if (auto dot = dynamic_pointer_cast<DotExpression>(e)) {
            auto& ch = dot->getChildren();
            if (ch.empty()) return false;
            if (dynamic_pointer_cast<ThisExpression>(ch[0])) return true;
            auto recv = dynamic_pointer_cast<IdentifierExpression>(ch[0]);
            return recv && recv->getTextValue() == "this";
        }
        return false;
    }

    // True for a kind-decidable title: a fresh value or a String concatenation.
    bool Method::exprIsStaticTitle(const ExpressionPtr& e) {
        if (!e) return false;
        if (auto ne = dynamic_pointer_cast<NewExpression>(e)) {
            return !ne->getStackAlloc() && !ne->getSharedAlloc();
        }
        if (auto ai = dynamic_pointer_cast<AggregateInitializerExpression>(e)) {
            return !ai->getStackAlloc();
        }
        if (auto al = dynamic_pointer_cast<ArrayLiteralExpression>(e)) {
            return !al->isStackAlloc() && !al->isArenaEligible();
        }
        if (auto bop = dynamic_pointer_cast<BinaryOpExpression>(e)) {
            if (bop->getBinaryOp() != BINARY_OP_ADD || bop->isArenaEligible()) return false;
            auto cls = dynamic_pointer_cast<CajetaClass>(bop->getResolvedType());
            return cls && cls->getQName()
                && cls->getQName()->getTypeName() == "String"
                && cls->getQName()->getPackageName() == "cajeta.lang";
        }
        if (auto tl = dynamic_pointer_cast<TextLiteralExpression>(e)) {
            return tl->getLiteralType() == LITERAL_TYPE_STRING
                || tl->getLiteralType() == LITERAL_TYPE_TEXT_BLOCK;
        }
        return false;
    }

    bool Method::nodeReturnsOnlyStaticTitles(const AbstractSyntaxNodePtr& node,
                                             bool& sawReturn) {
        if (!node) return true;
        if (auto ret = dynamic_pointer_cast<ReturnStatement>(node)) {
            ExpressionPtr e = ret->getExpression();
            if (!e) return true;                       // bare `return;`
            if (ret->isModeCarrying()) return false;   // `return #= x`
            if (!exprIsStaticTitle(e)) return false;
            sawReturn = true;
            return true;
        }
        if (auto lbl = dynamic_pointer_cast<LabelStatement>(node)) {
            return nodeReturnsOnlyStaticTitles(lbl->getBlock(), sawReturn);
        }
        if (auto sc = dynamic_pointer_cast<ScopeStatement>(node)) {
            return nodeReturnsOnlyStaticTitles(sc->getBlock(), sawReturn);
        }
        if (auto iff = dynamic_pointer_cast<IfStatement>(node)) {
            return nodeReturnsOnlyStaticTitles(iff->getThenBranch(), sawReturn)
                && nodeReturnsOnlyStaticTitles(iff->getElseBranch(), sawReturn);
        }
        if (auto wh = dynamic_pointer_cast<WhileStatement>(node)) {
            return nodeReturnsOnlyStaticTitles(wh->getBody(), sawReturn);
        }
        if (auto fr = dynamic_pointer_cast<ForStatement>(node)) {
            return nodeReturnsOnlyStaticTitles(fr->getBody(), sawReturn);
        }
        if (auto efr = dynamic_pointer_cast<EnhancedForStatement>(node)) {
            return nodeReturnsOnlyStaticTitles(efr->getBody(), sawReturn);
        }
        if (auto dod = dynamic_pointer_cast<DoStatement>(node)) {
            return nodeReturnsOnlyStaticTitles(dod->getBody(), sawReturn);
        }
        for (auto& c : node->getChildren()) {
            if (!nodeReturnsOnlyStaticTitles(c, sawReturn)) return false;
        }
        return true;
    }

    bool Method::returnsStaticTitle() const {
        if (!returnsOwnership || !block) return false;
        bool sawReturn = false;
        if (!nodeReturnsOnlyStaticTitles(block, sawReturn)) return false;
        return sawReturn;
    }

    bool Method::returnsInteriorView() const {
        if (returnsOwnership || !block) return false;
        bool sawView = false;
        if (!nodeReturnsOnlyInteriorViews(block, sawView)) return false;
        return sawView;
    }

    // True while every return is an interior read or a bare `null`; descends the
    // branch and loop bodies that are not reachable through `children`.
    bool Method::nodeReturnsOnlyInteriorViews(const AbstractSyntaxNodePtr& node,
                                              bool& sawView) {
        if (!node) return true;
        if (auto ret = dynamic_pointer_cast<ReturnStatement>(node)) {
            ExpressionPtr e = ret->getExpression();
            if (!e) return true;                       // bare `return;`
            if (auto tl = dynamic_pointer_cast<TextLiteralExpression>(e)) {
                if (tl->getLiteralType() == LITERAL_TYPE_NULL) return true;
            }
            if (!exprIsInteriorRead(e)) return false;
            sawView = true;
            return true;
        }
        if (auto lbl = dynamic_pointer_cast<LabelStatement>(node)) {
            return nodeReturnsOnlyInteriorViews(lbl->getBlock(), sawView);
        }
        if (auto sc = dynamic_pointer_cast<ScopeStatement>(node)) {
            return nodeReturnsOnlyInteriorViews(sc->getBlock(), sawView);
        }
        if (auto iff = dynamic_pointer_cast<IfStatement>(node)) {
            return nodeReturnsOnlyInteriorViews(iff->getThenBranch(), sawView)
                && nodeReturnsOnlyInteriorViews(iff->getElseBranch(), sawView);
        }
        if (auto wh = dynamic_pointer_cast<WhileStatement>(node)) {
            return nodeReturnsOnlyInteriorViews(wh->getBody(), sawView);
        }
        if (auto fr = dynamic_pointer_cast<ForStatement>(node)) {
            return nodeReturnsOnlyInteriorViews(fr->getBody(), sawView);
        }
        if (auto efr = dynamic_pointer_cast<EnhancedForStatement>(node)) {
            return nodeReturnsOnlyInteriorViews(efr->getBody(), sawView);
        }
        if (auto dod = dynamic_pointer_cast<DoStatement>(node)) {
            return nodeReturnsOnlyInteriorViews(dod->getBody(), sawView);
        }
        for (auto& c : node->getChildren()) {
            if (!nodeReturnsOnlyInteriorViews(c, sawView)) return false;
        }
        return true;
    }

    bool Method::blockHasStackReturn(const BlockPtr& block) {
        if (!block) return false;
        for (auto& child : block->getChildren()) {
            if (nodeHasStackReturn(child)) return true;
        }
        return false;
    }

    // True when the body contains any `return stack X(...)`. Branch/loop bodies sit
    // behind accessors rather than in `children`, so they are descended explicitly.
    bool Method::nodeHasStackReturn(const AbstractSyntaxNodePtr& node) {
        if (!node) return false;
        if (auto ret = dynamic_pointer_cast<ReturnStatement>(node)) {
            return exprIsStackConstruction(ret->getExpression());
        }
        if (auto lbl = dynamic_pointer_cast<LabelStatement>(node)) {
            return blockHasStackReturn(lbl->getBlock());
        }
        if (auto sc = dynamic_pointer_cast<ScopeStatement>(node)) {
            return blockHasStackReturn(sc->getBlock());
        }
        if (auto iff = dynamic_pointer_cast<IfStatement>(node)) {
            return nodeHasStackReturn(iff->getThenBranch())
                || nodeHasStackReturn(iff->getElseBranch());
        }
        if (auto wh = dynamic_pointer_cast<WhileStatement>(node)) {
            return nodeHasStackReturn(wh->getBody());
        }
        if (auto fr = dynamic_pointer_cast<ForStatement>(node)) {
            return nodeHasStackReturn(fr->getBody());
        }
        if (auto efr = dynamic_pointer_cast<EnhancedForStatement>(node)) {
            return nodeHasStackReturn(efr->getBody());
        }
        if (auto dod = dynamic_pointer_cast<DoStatement>(node)) {
            return nodeHasStackReturn(dod->getBody());
        }
        for (auto& c : node->getChildren()) {
            if (nodeHasStackReturn(c)) return true;
        }
        return false;
    }

    // --- Lint helpers for [heap-optional-return] --------------------------
    static std::string trimTemplateArgs(const std::string& canonical) {
        auto lt = canonical.find('<');
        if (lt == std::string::npos) return canonical;
        return canonical.substr(0, lt);
    }
    // True when `e` is a `heap C(...)` whose constructed class canonical (template
    // arguments trimmed) equals `targetClassCanonical`.
    static bool exprIsHeapCtorOfClass(const ExpressionPtr& e,
            const std::string& targetClassCanonical) {
        if (!e) return false;
        auto ne = dynamic_pointer_cast<NewExpression>(e);
        if (!ne) return false;
        if (ne->getStackAlloc()) return false;
        auto rt = ne->getResolvedType();
        if (!rt || !rt->getQName()) return false;
        std::string ctorClass = trimTemplateArgs(
            rt->getQName()->toCanonical());
        return ctorClass == targetClassCanonical;
    }
    // Visit every return statement in the body with `visit(retExpr)`, stopping at
    // the first false. Same structural walk as nodeHasStackReturn.
    static bool methodVisitReturns(const AbstractSyntaxNodePtr& node,
            const std::function<bool(const ExpressionPtr&)>& visit);
    static bool methodVisitReturnsBlock(const BlockPtr& block,
            const std::function<bool(const ExpressionPtr&)>& visit) {
        if (!block) return true;
        for (auto& c : block->getChildren()) {
            if (!methodVisitReturns(c, visit)) return false;
        }
        return true;
    }
    static bool methodVisitReturns(const AbstractSyntaxNodePtr& node,
            const std::function<bool(const ExpressionPtr&)>& visit) {
        if (!node) return true;
        if (auto ret = dynamic_pointer_cast<ReturnStatement>(node)) {
            return visit(ret->getExpression());
        }
        if (auto lbl = dynamic_pointer_cast<LabelStatement>(node)) {
            return methodVisitReturnsBlock(lbl->getBlock(), visit);
        }
        if (auto sc = dynamic_pointer_cast<ScopeStatement>(node)) {
            return methodVisitReturnsBlock(sc->getBlock(), visit);
        }
        if (auto iff = dynamic_pointer_cast<IfStatement>(node)) {
            return methodVisitReturns(iff->getThenBranch(), visit)
                && methodVisitReturns(iff->getElseBranch(), visit);
        }
        if (auto wh = dynamic_pointer_cast<WhileStatement>(node)) {
            return methodVisitReturns(wh->getBody(), visit);
        }
        if (auto fr = dynamic_pointer_cast<ForStatement>(node)) {
            return methodVisitReturns(fr->getBody(), visit);
        }
        if (auto efr = dynamic_pointer_cast<EnhancedForStatement>(node)) {
            return methodVisitReturns(efr->getBody(), visit);
        }
        if (auto dod = dynamic_pointer_cast<DoStatement>(node)) {
            return methodVisitReturns(dod->getBody(), visit);
        }
        for (auto& c : node->getChildren()) {
            if (!methodVisitReturns(c, visit)) return false;
        }
        return true;
    }

    // Collect the body locals whose initializer is a heap allocation — `T x = heap
    // ...` or `T x #= heap ...`, whose `#=` spelling wraps the RHS in a
    // MoveExpression. These are the names a `return x` can yield a title through.
    static void collectHeapInitLocals(const AbstractSyntaxNodePtr& node,
            std::unordered_set<std::string>& out) {
        if (!node) return;
        if (auto lvd = dynamic_pointer_cast<LocalVariableDeclaration>(node)) {
            for (auto& d : lvd->getVariableDeclarators()) {
                if (!d) continue;
                auto vi = dynamic_pointer_cast<VariableInitializer>(
                    d->getInitializer());
                if (!vi || vi->getChildren().empty()) continue;
                AbstractSyntaxNodePtr init = vi->getChildren()[0];
                if (auto mv = dynamic_pointer_cast<MoveExpression>(init)) {
                    if (!mv->getChildren().empty()) init = mv->getChildren()[0];
                }
                if (auto ne = dynamic_pointer_cast<NewExpression>(init)) {
                    if (!ne->getStackAlloc()) out.insert(d->getIdentifier());
                }
            }
            return;
        }
        if (auto lbl = dynamic_pointer_cast<LabelStatement>(node)) {
            if (lbl->getBlock())
                for (auto& c : lbl->getBlock()->getChildren())
                    collectHeapInitLocals(c, out);
            return;
        }
        if (auto sc = dynamic_pointer_cast<ScopeStatement>(node)) {
            if (sc->getBlock())
                for (auto& c : sc->getBlock()->getChildren())
                    collectHeapInitLocals(c, out);
            return;
        }
        if (auto iff = dynamic_pointer_cast<IfStatement>(node)) {
            collectHeapInitLocals(iff->getThenBranch(), out);
            collectHeapInitLocals(iff->getElseBranch(), out);
            return;
        }
        if (auto wh = dynamic_pointer_cast<WhileStatement>(node)) {
            collectHeapInitLocals(wh->getBody(), out);
            return;
        }
        if (auto fr = dynamic_pointer_cast<ForStatement>(node)) {
            collectHeapInitLocals(fr->getBody(), out);
            return;
        }
        if (auto efr = dynamic_pointer_cast<EnhancedForStatement>(node)) {
            collectHeapInitLocals(efr->getBody(), out);
            return;
        }
        if (auto dod = dynamic_pointer_cast<DoStatement>(node)) {
            collectHeapInitLocals(dod->getBody(), out);
            return;
        }
        for (auto& c : node->getChildren()) collectHeapInitLocals(c, out);
    }

    void Method::lintPlainReturnYieldsTitle() {
        if (returnsOwnership || returnsView || !block) return;
        auto rtClass = dynamic_pointer_cast<CajetaClass>(returnType);
        // Reference-semantics returns only: value types return by copy and
        // shared-capable values move out by share-bump, and FRESH_RETURN exempts both.
        if (!rtClass || rtClass->isValueType()
                || rtClass->isSharedCapableValue()) return;
        static thread_local std::unordered_set<std::string> warnedPlain;
        std::string parentCanonical = parent && parent->getQName()
            ? trimTemplateArgs(parent->getQName()->toCanonical())
            : std::string("");
        std::string key = parentCanonical + "::" + name;
        if (warnedPlain.count(key)) return;
        std::unordered_set<std::string> heapLocals;
        for (auto& c : block->getChildren()) collectHeapInitLocals(c, heapLocals);
        int returnCount = 0;
        bool allYieldTitle = methodVisitReturnsBlock(block,
            [&](const ExpressionPtr& e) -> bool {
                returnCount++;
                if (!e) return false;
                if (auto ne = dynamic_pointer_cast<NewExpression>(e)) {
                    return !ne->getStackAlloc();
                }
                if (auto id = dynamic_pointer_cast<IdentifierExpression>(e)) {
                    return heapLocals.count(id->getTextValue()) > 0;
                }
                return false;
            });
        if (!allYieldTitle || returnCount == 0) return;
        warnedPlain.insert(key);
        std::ostringstream w;
        w << "warning: [plain-return-yields-title] "
            << (parentCanonical.empty()
                ? std::string("") : parentCanonical + ".")
            << name
            << " declares a plain return but every return hands out a "
            << "fresh heap allocation — the caller receives a title the "
            << "signature does not declare. Declare the return `#"
            << (returnType && returnType->getQName()
                ? returnType->getQName()->getTypeName() : std::string("T"))
            << "` (codegen will reject this shape anyway: "
            << "CAJETA_ERROR_FRESH_RETURN_NEEDS_TRANSFER).\n";
        logLine("warn", w.str());
    }

    // [plain-return-of-owned-slot] helpers: track which `a[i]` slots the body
    // provably armed (`#=` into a frame-owned array) and fire on a plain return of
    // one. The proof is structural dominance; conditional arming is discarded.
    namespace {
        struct SlotArmEnv {
            std::unordered_set<std::string> ownedArrays;
            // "name\x1f<index literal>" — armed slots provable at this point.
            std::unordered_set<std::string> armed;
        };

        std::string slotKeyOf(const ExpressionPtr& e) {
            auto aix = dynamic_pointer_cast<ArrayIndexExpression>(e);
            if (!aix || aix->getChildren().size() < 2) return "";
            auto base = dynamic_pointer_cast<IdentifierExpression>(
                aix->getChildren()[0]);
            auto idx = dynamic_pointer_cast<IntegerLiteralExpression>(
                aix->getChildren()[1]);
            if (!base || !idx) return "";
            return base->getTextValue() + "\x1f" + idx->getRawValue();
        }

        // Walk statements tracking the armed slots, per branch, and fire on a plain
        // return of one. `env` is copied into each branch/loop body.
        void walkSlotArm(const AbstractSyntaxNodePtr& node, SlotArmEnv& env,
                const std::function<void(const std::string&, int)>& fire) {
            if (!node) return;
            if (auto lvd = dynamic_pointer_cast<LocalVariableDeclaration>(node)) {
                for (auto& d : lvd->getVariableDeclarators()) {
                    if (!d) continue;
                    auto vi = dynamic_pointer_cast<VariableInitializer>(
                        d->getInitializer());
                    if (!vi || vi->getChildren().empty()) continue;
                    AbstractSyntaxNodePtr init = vi->getChildren()[0];
                    if (auto mv = dynamic_pointer_cast<MoveExpression>(init)) {
                        if (!mv->getChildren().empty())
                            init = mv->getChildren()[0];
                    }
                    if (auto ne = dynamic_pointer_cast<NewExpression>(init)) {
                        if (!ne->getStackAlloc()
                                && dynamic_pointer_cast<ArrayCreatorRest>(
                                       ne->getCreatorRest())) {
                            env.ownedArrays.insert(d->getIdentifier());
                        }
                    }
                }
                return;
            }
            // ExpressionStatement holds its expression in a private slot, not in
            // `children` — forward explicitly or every assignment is invisible here.
            if (auto es = dynamic_pointer_cast<ExpressionStatement>(node)) {
                walkSlotArm(es->getExpression(), env, fire);
                return;
            }
            if (auto bop = dynamic_pointer_cast<BinaryOpExpression>(node)) {
                if (bop->getBinaryOp() == BINARY_OP_ASSIGN
                        && bop->getChildren().size() >= 2) {
                    std::string key = slotKeyOf(dynamic_pointer_cast<Expression>(
                        bop->getChildren()[0]));
                    if (!key.empty()) {
                        std::string base = key.substr(0, key.find('\x1f'));
                        AbstractSyntaxNodePtr rhs = bop->getChildren()[1];
                        bool arms = false;
                        if (dynamic_pointer_cast<MoveExpression>(rhs)) {
                            arms = true;    // the `#=` desugar wraps its RHS
                        } else if (auto rn =
                                dynamic_pointer_cast<NewExpression>(rhs)) {
                            arms = !rn->getStackAlloc();
                        }
                        if (arms && env.ownedArrays.count(base)) {
                            env.armed.insert(key);
                        } else {
                            env.armed.erase(key);   // plain store lends
                        }
                        return;
                    }
                }
            }
            if (auto ret = dynamic_pointer_cast<ReturnStatement>(node)) {
                ExpressionPtr e = ret->getExpression();
                while (auto cast = dynamic_pointer_cast<CastExpression>(e)) {
                    e = cast->getChildren().empty() ? nullptr
                        : dynamic_pointer_cast<Expression>(
                              cast->getChildren()[0]);
                }
                std::string key = slotKeyOf(e);
                if (!key.empty() && env.armed.count(key)) {
                    fire(key.substr(0, key.find('\x1f')),
                         (int) ret->getSourceLine());
                }
                return;
            }
            if (auto lbl = dynamic_pointer_cast<LabelStatement>(node)) {
                if (lbl->getBlock())
                    for (auto& c : lbl->getBlock()->getChildren())
                        walkSlotArm(c, env, fire);
                return;
            }
            if (auto sc = dynamic_pointer_cast<ScopeStatement>(node)) {
                if (sc->getBlock())
                    for (auto& c : sc->getBlock()->getChildren())
                        walkSlotArm(c, env, fire);
                return;
            }
            if (auto iff = dynamic_pointer_cast<IfStatement>(node)) {
                SlotArmEnv thenEnv = env;
                walkSlotArm(iff->getThenBranch(), thenEnv, fire);
                SlotArmEnv elseEnv = env;
                walkSlotArm(iff->getElseBranch(), elseEnv, fire);
                return;
            }
            if (auto wh = dynamic_pointer_cast<WhileStatement>(node)) {
                SlotArmEnv c = env; walkSlotArm(wh->getBody(), c, fire);
                return;
            }
            if (auto fr = dynamic_pointer_cast<ForStatement>(node)) {
                SlotArmEnv c = env; walkSlotArm(fr->getBody(), c, fire);
                return;
            }
            if (auto efr = dynamic_pointer_cast<EnhancedForStatement>(node)) {
                SlotArmEnv c = env; walkSlotArm(efr->getBody(), c, fire);
                return;
            }
            if (auto dod = dynamic_pointer_cast<DoStatement>(node)) {
                SlotArmEnv c = env; walkSlotArm(dod->getBody(), c, fire);
                return;
            }
            for (auto& c : node->getChildren()) walkSlotArm(c, env, fire);
        }
    }  // namespace

    void Method::lintPlainReturnOfOwnedSlot() {
        if (returnsOwnership || returnsView || !block) return;
        auto rtClass = dynamic_pointer_cast<CajetaClass>(returnType);
        if (!rtClass || rtClass->isValueType()
                || rtClass->isSharedCapableValue()) return;
        static thread_local std::unordered_set<std::string> warnedSlot;
        std::string parentCanonical = parent && parent->getQName()
            ? trimTemplateArgs(parent->getQName()->toCanonical())
            : std::string("");
        std::string key = parentCanonical + "::" + name;
        if (warnedSlot.count(key)) return;
        SlotArmEnv env;
        bool fired = false;
        for (auto& c : block->getChildren()) {
            walkSlotArm(c, env, [&](const std::string& arr, int line) {
                if (fired) return;
                fired = true;
                warnedSlot.insert(key);
                std::ostringstream w;
                w << "warning: [plain-return-of-owned-slot] "
                    << (parentCanonical.empty()
                        ? std::string("") : parentCanonical + ".")
                    << name << ":" << line
                    << " returns a borrow of a slot this method owns — `"
                    << arr << "` is a frame-owned array and this body armed "
                    << "the returned element (`#=`), so the array's "
                    << "scope-exit drop frees it before the caller can "
                    << "read it. Extract the title instead: declare the "
                    << "return `#"
                    << (returnType && returnType->getQName()
                        ? returnType->getQName()->getTypeName()
                        : std::string("T"))
                    << "` and write `return #" << arr << "[...]`, or store "
                    << "the element un-owned (plain `=`) if the array was "
                    << "never meant to own it.\n";
                logLine("warn", w.str());
            });
        }
    }

    void Method::lintHeapOptionalReturn() {
        if (!returnsOwnership) return;
        auto rtClass = dynamic_pointer_cast<CajetaClass>(returnType);
        if (!rtClass || !rtClass->getQName()) return;
        std::string rtCanonical = trimTemplateArgs(
            rtClass->getQName()->toCanonical());
        if (rtCanonical != "cajeta.lang.Optional") return;
        if (findAnnotation("HeapReturn") != nullptr) return;
        if (!block) return;
        // Dedupe by source canonical (template arguments trimmed) so one declaration
        // warns once, not once per template instantiation.
        static thread_local std::unordered_set<std::string> warned;
        std::string parentCanonical = parent && parent->getQName()
            ? trimTemplateArgs(parent->getQName()->toCanonical())
            : std::string("");
        std::string key = parentCanonical + "::" + name;
        if (warned.count(key)) return;
        int returnCount = 0;
        bool allHeapOptional = methodVisitReturnsBlock(block,
            [&](const ExpressionPtr& e) -> bool {
                returnCount++;
                return exprIsHeapCtorOfClass(e, "cajeta.lang.Optional");
            });
        if (!allHeapOptional || returnCount == 0) return;
        warned.insert(key);
        std::ostringstream w;
        w << "warning: [heap-optional-return] "
            << (parentCanonical.empty()
                ? std::string("") : parentCanonical + ".")
            << name
            << " returns #Optional<...> but every return is a "
            << "scope-bounded `heap Optional<...>(...)`. Consider "
            << "dropping `#` from the return type and switching to "
            << "`return stack Optional<...>(...)` — the value lands "
            << "in the caller's slot by copy (sret), avoiding a heap "
            << "allocation per call. Suppress with @HeapReturn when "
            << "the caller really needs heap ownership.\n";
        logLine("warn", w.str());
    }

    bool Method::needsTransferWord() {
        // Boundaries the compiler does not own both sides of keep the plain C ABI: a
        // @Native wrapper derives its C extern's type from this signature, so a
        // trailing word here would mis-declare the runtime symbol.
        if (findAnnotation("Kernel") || findAnnotation("Device")
                || findAnnotation("Native")) {
            return false;
        }
        bool staticMethod = modifiers.find(STATIC) != modifiers.end();
        if (staticMethod && name == "main") {
            return false;
        }
        for (auto& formalParameter : parameterList) {
            if (!formalParameter || formalParameter->getName() == "this") {
                continue;
            }
            CajetaTypePtr pt = formalParameter->getType();
            bool isArr = dynamic_pointer_cast<CajetaArray>(pt) != nullptr;
            bool isClassLike = dynamic_pointer_cast<CajetaClass>(pt) != nullptr;
            bool isPrim = pt && (pt->getTypeFlags() & PRIMITIVE_FLAG);
            if (isClassLike && (isArr || !isPrim) && !pt->isValueType()) {
                return true;
            }
            if (dynamic_pointer_cast<CajetaFunctionType>(pt)) {
                return true;
            }
        }
        return false;
    }

    bool Method::returnsClassPointer() {
        // @Native bodies are C and never store the return flag, so a class-pointer
        // result from one is a borrow at the flag layer until cajeta code wraps it.
        if (findAnnotation("Kernel") || findAnnotation("Device")
                || findAnnotation("Native")) {
            return false;
        }
        CajetaTypePtr rt = returnType;
        bool isArrR = rt && dynamic_pointer_cast<CajetaArray>(rt) != nullptr;
        auto rtClass = dynamic_pointer_cast<CajetaClass>(rt);
        bool isPrimR = rt && (rt->getTypeFlags() & PRIMITIVE_FLAG);
        bool isInterfaceR = rtClass && rtClass->isInterface();
        bool isValueTypeR = rt && rt->isValueType();
        return rtClass != nullptr && (isArrR || !isPrimR) && !isInterfaceR
            && !isValueTypeR && !returnsStackValue();
    }

    CajetaModulePtr Method::getEmitModule() {
        if (emitModule) return emitModule;
        if (parent) {
            if (CajetaModulePtr pm = parent->getEmitModule()) {
                if (pm != module && parent->hasEmitModuleOverride()) return pm;
            }
        }
        return module;
    }

    void Method::emitFormalDropEntries(CajetaModulePtr module) {
        llvm::Value* word = getTransferWordArg();
        if (!word) return;
        auto* builder = module->getBuilder();
        auto& ctx = *module->getLlvmContext();
        llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        bool debug = module->getFlags().sourceTags;
        llvm::Function* pushFn = module->getRuntimeFunction(
            debug ? "__cajeta_drop_push_flag_debug" : "__cajeta_drop_push_flag");
        if (!pushFn) return;
        unsigned entryBytes = debug ? 40 : 32;
        auto scope = module->getScopeStack().peek();
        if (!scope) return;
        int bit = -1;
        for (auto& formalParameter : parameterList) {
            if (!formalParameter || formalParameter->getName() == "this") {
                continue;
            }
            if (++bit >= 64) break;
            CajetaTypePtr pt = formalParameter->getType();
            auto arr = dynamic_pointer_cast<CajetaArray>(pt);
            auto klass = dynamic_pointer_cast<CajetaClass>(pt);
            const char* dropFnName = nullptr;
            if (arr) {
                if (!arr->isInlineArray()) dropFnName = "__cajeta_free_array";
            } else if (klass && !dynamic_pointer_cast<CajetaView>(pt)
                    && !klass->isInterface() && !pt->isValueType()
                    && !klass->isSharedCapableValue()
                    && klass->hasVtablePointerAtSlotZero()) {
                // String needs its OWN exclusion: isSharedCapableValue is a
                // value-type predicate and misses String-the-class, whose title moves
                // by share/resolve — an entry here virtual-drops a live String.
                bool isLangString = klass->getQName()
                    && klass->getQName()->getTypeName() == "String"
                    && klass->getQName()->getPackageName() == "cajeta.lang";
                if (!isLangString) {
                    klass->patchVirtualTableDropFn();
                    dropFnName = "__cajeta_class_virtual_drop";
                }
            }
            if (!dropFnName && dynamic_pointer_cast<CajetaFunctionType>(pt)) {
                dropFnName = "__cajeta_closure_drop";
            }
            if (!dropFnName) continue;
            llvm::Function* dropFn = module->getRuntimeFunction(dropFnName);
            if (!dropFn) continue;
            FieldPtr pf = scope->getField(formalParameter->getName());
            if (!pf || pf->getDropEntry()) continue;
            llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
            llvm::IRBuilder<> entryBuilder(&parentFn->getEntryBlock(),
                parentFn->getEntryBlock().begin());
            llvm::Value* entryPtr = entryBuilder.CreateAlloca(
                llvm::ArrayType::get(i8Ty, entryBytes));
            llvm::Value* obj = builder->CreateLoad(
                ptrTy, pf->getOrCreateAllocation());
            llvm::Value* flag = builder->CreateAnd(
                builder->CreateLShr(word,
                    llvm::ConstantInt::get(i64Ty, bit)),
                llvm::ConstantInt::get(i64Ty, 1), "formal_title");
            if (debug) {
                llvm::Constant* fileConst =
                    module->getOrCreateSourceFileConstant(
                        module->getSourcePath());
                llvm::Constant* lineConst = llvm::ConstantInt::get(i32Ty, 0);
                builder->CreateCall(pushFn,
                    {entryPtr, obj, dropFn, fileConst, lineConst, flag});
            } else {
                builder->CreateCall(pushFn, {entryPtr, obj, dropFn, flag});
            }
            pf->setDropEntry(entryPtr);
            pf->setRuntimeConditionalOwner(true);
            registerDropEntry(entryPtr);
        }
    }

    bool Method::returnsStackValue() {
        if (returnsStackValueCache != -1) {
            return returnsStackValueCache == 1;
        }
        returnsStackValueCache = 0;
        if (returnsOwnership || returnsView) return false;
        if (returnType && returnType->isValueType()) return false;
        auto rtClass = dynamic_pointer_cast<CajetaClass>(returnType);
        if (!rtClass) return false;
        if (rtClass->isInterface()) return false;
        if (dynamic_pointer_cast<CajetaArray>(returnType)) return false;
        if (blockHasStackReturn(block)) {
            returnsStackValueCache = 1;
            return true;
        }
        // Force sret ONLY for the known value-shape class: every other class return
        // uses the reference (heap-pointer) ABI, and sret-forcing one turns the sret
        // slot into `this` at the call site. Decided by TYPE, so relays agree too.
        if (isValueShapeReturnType(rtClass)) {
            if (getenv("CAJETA_DBG_SRET")) {
                std::cerr << "[sret-probe] value-shape return "
                          << (parent && parent->getQName()
                                  ? parent->getQName()->toCanonical() : std::string("?"))
                          << "::" << name << " rt=" << rtClass->toCanonical()
                          << (parent && parent->isInterface() ? " (iface decl)" : "")
                          << std::endl;
            }
            returnsStackValueCache = 1;
            return true;
        }
        // An override must match an ancestor's sret ABI or virtual dispatch misaligns
        // its arguments. Match by name + parameter count — the discriminator vtable
        // slot resolution uses — since the map key is the full canonical signature.
        if (parent && !parent->isInterface()) {
            size_t myParamCount = parameterList.size();
            for (auto& ancestor : parent->getSuperClasses()) {
                CajetaClassPtr cls = ancestor;
                while (cls) {
                    for (auto& kv : cls->getMethods()) {
                        MethodPtr ancMethod = kv.second;
                        if (!ancMethod || ancMethod.get() == this) continue;
                        if (ancMethod->getName() != name) continue;
                        if (ancMethod->getParameterList().size() != myParamCount) continue;
                        if (ancMethod->returnsStackValue()) {
                            returnsStackValueCache = 1;
                            return true;
                        }
                    }
                    if (cls->getSuperClasses().empty()) break;
                    cls = cls->getSuperClasses().front();
                }
            }
        }
        return false;
    }

    void Method::recordNativeRequirement(const std::string& lib,
                                         const std::string& symbol) {
        llvm::Module* lmod = getEmitModule()->getLlvmModule();
        llvm::LLVMContext& ctx = lmod->getContext();
        llvm::NamedMDNode* nmd =
            lmod->getOrInsertNamedMetadata("cajeta.native.reqs");
        for (unsigned i = 0; i < nmd->getNumOperands(); ++i) {
            llvm::MDNode* op = nmd->getOperand(i);
            if (op->getNumOperands() != 2) continue;
            auto* l = llvm::dyn_cast<llvm::MDString>(op->getOperand(0));
            auto* s = llvm::dyn_cast<llvm::MDString>(op->getOperand(1));
            if (l && s && l->getString() == lib && s->getString() == symbol)
                return;
        }
        llvm::Metadata* entry[] = {
            llvm::MDString::get(ctx, lib),
            llvm::MDString::get(ctx, symbol),
        };
        nmd->addOperand(llvm::MDNode::get(ctx, entry));
    }

    void Method::emitNativeForwardingBody(const std::string& symbol) {
        auto& llvmFunction = llvmFunctionRef();
        auto& llvmFunctionType = llvmFunctionTypeRef();
        // The forwarding wrapper IS this method's llvmFunction, so the runtime
        // symbol's extern declaration must be co-resident in the emit module.
        llvm::Module* lmod = getEmitModule()->getLlvmModule();
        llvm::LLVMContext& ctx = *module->getLlvmContext();

        // fp128 @Native parameters are forwarded BY POINTER. An i128-by-value param
        // is lowered indirectly on Win64 (`i64(ptr dead_on_return)`) and the call then
        // mismatches the C definition; by pointer it is `i64(ptr)` everywhere.
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto coerceTy = [&](llvm::Type* t) -> llvm::Type* {
            return t->isFP128Ty() ? ptrTy : t;
        };

        std::vector<llvm::Type*> targetParams;
        targetParams.reserve(llvmFunctionType->getNumParams());
        for (llvm::Type* pt : llvmFunctionType->params()) {
            targetParams.push_back(coerceTy(pt));
        }
        llvm::FunctionType* targetFnType = llvm::FunctionType::get(
            llvmFunctionType->getReturnType(), targetParams,
            llvmFunctionType->isVarArg());

        llvm::Function* targetFn = lmod->getFunction(symbol);
        if (!targetFn) {
            targetFn = llvm::Function::Create(
                targetFnType,
                llvm::Function::ExternalLinkage,
                symbol,
                lmod);
        }

        llvmBasicBlock = llvm::BasicBlock::Create(
            ctx, "entry", llvmFunction);
        llvm::IRBuilder<> b(llvmBasicBlock);

        std::vector<llvm::Value*> args;
        args.reserve(llvmFunction->arg_size());
        // @Native array ABI: an `int8[]` crosses as its HEADER pointer
        // ({ i64 count, [N x i8] data }) and the C bridge skips the 8-byte header
        // itself, so arguments forward verbatim — except the fp128 spill above.
        for (auto& arg : llvmFunction->args()) {
            if (arg.getType()->isFP128Ty()) {
                llvm::Value* slot = b.CreateAlloca(arg.getType());
                b.CreateStore(&arg, slot);
                args.push_back(slot);
            } else {
                args.push_back(&arg);
            }
        }

        if (llvmFunction->getReturnType()->isVoidTy()) {
            b.CreateCall(targetFn, args);
            b.CreateRetVoid();
        } else {
            llvm::Value* result = b.CreateCall(targetFn, args);
            b.CreateRet(result);
        }
    }

    void Method::emitTopFrameDrops(CajetaModulePtr module) {
        if (dropFrameStack.empty() || dropFrameStack.back().empty()) return;
        llvm::Function* popRun = module->getRuntimeFunction("__cajeta_drop_pop_run");
        if (!popRun) return;
        auto* b = module->getBuilder();
        auto& frame = dropFrameStack.back();
        for (auto it = frame.rbegin(); it != frame.rend(); ++it) {
            b->CreateCall(popRun, {*it});
        }
    }

    void Method::emitFrameDropsToDepth(CajetaModulePtr module, size_t keep) {
        if (dropFrameStack.size() <= keep) return;
        llvm::Function* popRun = module->getRuntimeFunction("__cajeta_drop_pop_run");
        if (!popRun) return;
        auto* b = module->getBuilder();
        // Every entry in these frames has provably executed its runtime push on any
        // path reaching here: codegen is linear and enclosing blocks ran in order.
        for (size_t fi = dropFrameStack.size(); fi > keep; --fi) {
            auto& frame = dropFrameStack[fi - 1];
            for (auto it = frame.rbegin(); it != frame.rend(); ++it) {
                b->CreateCall(popRun, {*it});
            }
        }
    }

    void Method::emitOwnerDrops(CajetaModulePtr module) {
        if (dropFrameStack.empty()) return;
        llvm::Function* popRun = module->getRuntimeFunction("__cajeta_drop_pop_run");
        if (!popRun) return;
        auto* b = module->getBuilder();
        for (auto fit = dropFrameStack.rbegin(); fit != dropFrameStack.rend(); ++fit) {
            for (auto eit = fit->rbegin(); eit != fit->rend(); ++eit) {
                b->CreateCall(popRun, {*eit});
            }
        }
        // Frame-arena: reclaim this method's arena locals after the drop chain, at
        // the method-entry mark, which dominates every return.
        if (methodArenaMark) {
            if (llvm::Function* resetFn = module->getRuntimeFunction("__cajeta_arena_reset")) {
                b->CreateCall(resetFn, {methodArenaMark});
            }
        }
    }

    // Emit one advice call. Advice that does not match the v1 shape (static, no
    // parameters, void return) is skipped rather than emitted as a bad call.
    static void emitOneAdviceCall(CajetaModulePtr module, const MethodPtr& advice) {
        if (!advice) return;
        llvm::Function* fn = advice->getLlvmFunction();
        if (!fn) return;
        if (fn->getFunctionType()->getNumParams() != 0) return;
        module->getBuilder()->CreateCall(fn, {});
    }

    void Method::emitBeforeAdvice(CajetaModulePtr module) {
        for (auto& m : matchingAdvice) {
            if (m.kind != AdviceKind::Before) continue;
            emitOneAdviceCall(module, m.adviceMethod);
        }
    }

    void Method::emitNonNullParamChecks(CajetaModulePtr module) {
        if (parameterList.empty()) return;
        auto& llvmFunction = llvmFunctionRef();

        llvm::IRBuilder<>* b = module->getBuilder();
        llvm::Function* throwFn = module->getRuntimeFunction("__cajeta_throw");
        if (!b || !throwFn) return;

        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);

        // sret methods prepend a hidden result pointer at LLVM arg 0, so formal i is
        // LLVM arg i+1 — without the shift a check reads the wrong argument.
        unsigned sretOffset = returnsStackValue() ? 1u : 0u;

        for (size_t i = 0; i < parameterList.size(); ++i) {
            auto& p = parameterList[i];
            if (!p) continue;
            if (p->getName() == "this") continue;
            // Parameter annotations go through the legacy annotation list, which
            // findAnnotation (annotationInstances) does not see — walk it directly.
            bool nn = false;
            for (auto& qn : p->getAnnotations()) {
                if (qn && qn->getTypeName() == "NonNull") { nn = true; break; }
            }
            if (!nn) continue;

            // Check the LLVM arg's own type: CajetaType::getLlvmType() can still hold
            // a placeholder shape here. Only pointer-typed args can be null.
            unsigned argIdx = (unsigned) i + sretOffset;
            if (argIdx >= llvmFunction->arg_size()) continue;
            llvm::Value* argVal = llvmFunction->getArg(argIdx);
            if (!argVal->getType()->isPointerTy()) continue;

            llvm::Value* isNull = b->CreateICmpEQ(
                argVal,
                llvm::ConstantPointerNull::get(ptrTy),
                std::string("nn.isnull.") + p->getName());

            llvm::Function* curFn = b->GetInsertBlock()->getParent();
            llvm::BasicBlock* throwBB = llvm::BasicBlock::Create(ctx,
                std::string("nn.throw.") + p->getName(), curFn);
            llvm::BasicBlock* okBB = llvm::BasicBlock::Create(ctx,
                std::string("nn.ok.") + p->getName(), curFn);
            b->CreateCondBr(isNull, throwBB, okBB);

            b->SetInsertPoint(throwBB);
            // Encode CAJETA_ERROR_NULL_PARAM_ARG = 2 as an IntToPtr, the same
            // integer-throw shape ThrowStatement::generateCode emits.
            llvm::Value* code = llvm::ConstantInt::get(i64Ty,
                llvm::APInt(64, 2, false));
            llvm::Value* ptrCode = b->CreateIntToPtr(code, ptrTy);
            b->CreateCall(throwFn, {ptrCode});
            b->CreateUnreachable();

            b->SetInsertPoint(okBB);
        }
    }

    void Method::emitAfterAdvice(CajetaModulePtr module) {
        for (auto& m : matchingAdvice) {
            if (m.kind != AdviceKind::After) continue;
            emitOneAdviceCall(module, m.adviceMethod);
        }
    }

    void Method::emitAfterReturningAdvice(CajetaModulePtr module) {
        for (auto& m : matchingAdvice) {
            if (m.kind != AdviceKind::AfterReturning) continue;
            emitOneAdviceCall(module, m.adviceMethod);
        }
    }

    void Method::emitAfterThrowingAdvice(CajetaModulePtr module) {
        for (auto& m : matchingAdvice) {
            if (m.kind != AdviceKind::AfterThrowing) continue;
            emitOneAdviceCall(module, m.adviceMethod);
        }
    }

    bool Method::hasAfterThrowingAdvice() const {
        for (auto& m : matchingAdvice) {
            if (m.kind == AdviceKind::AfterThrowing) return true;
        }
        return false;
    }

    Method::TryFrameInfo Method::emitAfterThrowingTryEntry(
            CajetaModulePtr module, llvm::IRBuilder<>& wb,
            llvm::Function* parentFn) {
        auto& ctx = *module->getLlvmContext();
        llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);

        TryFrameInfo info{nullptr, nullptr, nullptr};
        llvm::Function* push = module->getRuntimeFunction("__cajeta_exc_push");
        if (!push) return info;

        constexpr unsigned frameBytes = 512;
        llvm::IRBuilder<> entryBuilder(
            &parentFn->getEntryBlock(),
            parentFn->getEntryBlock().begin());
        llvm::AllocaInst* frameAlloca = entryBuilder.CreateAlloca(
            llvm::ArrayType::get(i8Ty, frameBytes));
        // 16-byte aligned: MSVCRT's _setjmp stores XMM registers into the
        // _JUMP_BUFFER with aligned stores (ExcFrameSetjmp.h).
        frameAlloca->setAlignment(llvm::Align(16));
        info.framePtr = frameAlloca;

        info.tryBB = llvm::BasicBlock::Create(ctx, "afterthrow_try", parentFn);
        info.catchBB = llvm::BasicBlock::Create(ctx, "afterthrow_catch", parentFn);

        wb.CreateCall(push, {info.framePtr});
        llvm::Value* sjResult = emitExcFrameSetjmp(wb, info.framePtr);
        llvm::Value* threw = wb.CreateICmpNE(sjResult,
            llvm::ConstantInt::get(i32Ty, 0));
        wb.CreateCondBr(threw, info.catchBB, info.tryBB);
        wb.SetInsertPoint(info.tryBB);
        return info;
    }

    void Method::emitAfterThrowingTryPop(CajetaModulePtr module) {
        if (!hasAfterThrowingAdvice()) return;
        auto& llvmOriginalFunction = llvmOriginalFunctionRef();
        // Only valid inside the BODY-level try frame: an @Around-wrapped method has
        // no frame at that level, and a stray pop would unbalance the caller's chain.
        if (llvmOriginalFunction) return;
        llvm::Function* pop = module->getRuntimeFunction("__cajeta_exc_pop");
        if (!pop) return;
        module->getBuilder()->CreateCall(pop, {});
    }

    void Method::emitAfterThrowingCatchArm(
            CajetaModulePtr module, llvm::IRBuilder<>& wb) {
        llvm::Function* getThrown = module->getRuntimeFunction("__cajeta_get_thrown");
        llvm::Function* pop = module->getRuntimeFunction("__cajeta_exc_pop");
        llvm::Function* throwFn = module->getRuntimeFunction("__cajeta_throw");
        if (!getThrown || !pop || !throwFn) return;

        llvm::Value* thrownVal = wb.CreateCall(getThrown, {}, "afterthrow_value");
        wb.CreateCall(pop, {});
        auto* prevBuilder = builder;
        auto* modPrev = module->getBuilder();
        builder = &wb;
        module->setBuilder(&wb);
        emitAfterThrowingAdvice(module);
        // @After fires on every exit path per the spec — the throw path included.
        emitAfterAdvice(module);
        builder = prevBuilder;
        module->setBuilder(modPrev);
        wb.CreateCall(throwFn, {thrownVal});
        wb.CreateUnreachable();
    }

    // Declare a local in the current scope. Throws when the name already exists.
    void Method::createLocalVariable(CajetaModulePtr module, FieldPtr field) {
        ScopePtr scope = module->getScopeStack().peek();
        if (scope->getField(field->getName()) != nullptr) {
            throw VariableAssignmentException(field->getName(),
                field->getType()->getQName()->toCanonical(),
                ERROR_CAUSE_VARIABLE_DUPLICATE,
                ERROR_ID_VARIABLE_DUPLICATE);
        }
        field->getOrCreateAllocation();
        scope->putField(field);
    }

    // Store `value` into an existing local. Throws when it is missing or final.
    void Method::setLocalVariable(CajetaModulePtr module, string name, llvm::Value* value) {
        ScopePtr scope = module->getScopeStack().peek();
        FieldPtr field = scope->getField(name);
        if (field == nullptr) {
            throw VariableAssignmentException(name,
                field->getType()->getQName()->toCanonical(),
                ERROR_CAUSE_VARIABLE_NOT_FOUND,
                ERROR_ID_VARIABLE_NOT_FOUND);
        }
        if (field->getModifiers().find(FINAL) != field->getModifiers().end()) {
            throw VariableAssignmentException(name,
                field->getType()->getQName()->toCanonical(),
                ERROR_CAUSE_ASSIGNMENT_FINAL,
                ERROR_ID_ASSIGNMENT_FINAL);
        }
        module->getBuilder()->CreateStore(value, field->getOrCreateAllocation());
    }

    void Method::ensureFreshPrototype() {
        if (llvmFunctionTypeRef() == nullptr) {
            generatePrototype();
            prototypeEpochSeen = CajetaClass::typeFillEpoch();
            return;
        }
        // A FROZEN method was prototyped against the fully-parsed prime world and can
        // never be epoch-stale; re-running corrupts state other sessions rely on.
        if (isFrozen()) return;
        uint64_t epoch = CajetaClass::typeFillEpoch();
        if (prototypeEpochSeen == epoch) return;
        // A placeholder filled since this prototype was built: the sret decision may
        // have been made against a placeholder body, so recompute it with the signature.
        returnsStackValueCache = -1;
        generatePrototype();
        prototypeEpochSeen = epoch;
    }

    // Build this method's LLVM FunctionType and Function — signature only, no body:
    // implicit `this`, the pass/return-by-pointer rules, the sret slot and the
    // trailing hidden transfer word. Idempotent: re-runs reuse the same Function.
    void Method::generatePrototype() {
        // A method template has no concrete signature until it is instantiated.
        if (isMethodTemplate()) return;
        // `^` stance-shape validation, hoisted ABOVE the abstract early-return so
        // interface declarations get it too. A `^` over a value-semantics return is
        // an error on a hand-written method, a demotion on a template instantiation.
        if (returnsView) {
            bool refTypedR = false;
            if (returnType) {
                if (auto rc = std::dynamic_pointer_cast<CajetaClass>(returnType)) {
                    if (!std::dynamic_pointer_cast<CajetaView>(returnType)
                            && !returnType->isValueType()) {
                        refTypedR = true;
                    }
                } else if (std::dynamic_pointer_cast<CajetaArray>(returnType)) {
                    refTypedR = true;
                }
            }
            const bool instantiated = isMethodTemplateInstantiation()
                || (parent && parent->isInstantiation());
            if (!refTypedR && returnType) {
                if (instantiated) {
                    returnsView = false;
                } else {
                    throw Exception(
                        "method '" + name + "' declares a `^` (view) return "
                        "over `" + returnType->toCanonical() + "`, which has "
                        "value semantics — the caller receives a copy, so "
                        "there is no interior to view and nothing the sigil "
                        "could protect. Fix: drop the `^` (a by-value return "
                        "needs no ownership marker). See "
                        "specs/stdlib-ownership-convention-spec.md §4.7.",
                        "CAJETA_ERROR_VIEW_RETURN_OF_VALUE");
                }
            }
            if (returnsView
                    && modifiers.find(STATIC) != modifiers.end()) {
                throw Exception(
                    "method '" + name + "' is STATIC and declares a `^` "
                    "(view) return — but a `^` result is interior to the "
                    "RECEIVER, and a static method has no receiver, so there "
                    "is no lifetime for the view to ride. Fix: make it an "
                    "instance method on the owning object, return `#` for a "
                    "fresh value, or use a plain return (spec §4.8's carry). "
                    "See specs/stdlib-ownership-convention-spec.md §4.7.",
                    "CAJETA_ERROR_VIEW_RETURN_STATIC");
            }
        }
        auto& llvmFunction = llvmFunctionRef();
        auto& llvmFunctionType = llvmFunctionTypeRef();
        // Emit-target swap (test-reuse): emit the Function and the runtime externs it
        // needs into the emit module. Resolution is by canonical name, so unaffected.
        CajetaModulePtr* moduleSlot = &module;
        CajetaModulePtr savedModule = module;
        { CajetaModulePtr em = getEmitModule();
          if (em && em != module) module = em; }
        struct RestoreModule {
            CajetaModulePtr* slot; CajetaModulePtr saved;
            ~RestoreModule() { *slot = saved; }
        } restoreModule{moduleSlot, savedModule};
        // Refresh returnType and the parameter types from canonicalMap: a signature
        // parsed inside `struct Foo` that names Foo holds a PLACEHOLDER class the real
        // registration later replaced, and a stale one picks the wrong return ABI.
        auto refreshType = [](CajetaTypePtr t) -> CajetaTypePtr {
            if (!t || !t->getQName()) return t;
            const std::string& canonical = t->getQName()->toCanonical();
            auto& cmap = CajetaType::getCanonicalMap();
            auto it = cmap.find(canonical);
            if (it != cmap.end() && it->second && it->second != t) {
                return it->second;
            }
            return t;
        };
        returnType = refreshType(returnType);
        for (auto& p : parameterList) {
            if (p) p->setType(refreshType(p->getType()));
        }

        vector<llvm::Type*> llvmTypes;
        // This runs several times per method, and each run would insert another
        // `this` — skip when one already sits at position 0. The rest is idempotent.
        bool thisAlreadyInserted = !parameterList.empty()
            && parameterList.front()->getName() == "this";

        // A @Native method may be declared with a `;` body, which leaves abstractFlag
        // set; demote it so the forwarding body gets a real LLVM function.
        bool isNative = findAnnotation("Native") != nullptr;
        if (abstractFlag && isNative) {
            abstractFlag = false;
        }

        // Abstract method: build the signature (callers need its canonical / vtable
        // hash and the indirect call type) but emit no LLVM function.
        if (abstractFlag) {
            bool staticAbstract = modifiers.find(STATIC) != modifiers.end();
            if (!staticAbstract && !thisAlreadyInserted) {
                auto thisParam = make_shared<FormalParameter>(string("this"), CajetaType::of("pointer"));
                thisParam->setParent(shared_from_this());
                parameterList.insert(parameterList.begin(), thisParam);
                parameters[thisParam->getName()] = thisParam;
            }
            // Same class-by-pointer / array-by-pointer rule as the concrete path
            // below: a templated-interface formal lowered to its inline struct type
            // makes the JIT verifier reject the call through the iface vtable.
            for (auto formalParameter: parameterList) {
                CajetaTypePtr pt = formalParameter->getType();
                llvm::Type* ptLlvm = pt->getLlvmType();
                bool isArr = dynamic_pointer_cast<CajetaArray>(pt) != nullptr;
                bool isClassLike = dynamic_pointer_cast<CajetaClass>(pt) != nullptr;
                bool isPrim = pt && (pt->getTypeFlags() & PRIMITIVE_FLAG);
                bool passByPointer = isClassLike && (isArr || !isPrim)
                    && !pt->isValueType();
                if (passByPointer) {
                    ptLlvm = llvm::PointerType::get(*module->getLlvmContext(), 0);
                }
                llvmTypes.push_back(ptLlvm);
            }
            // Mirror the concrete path's trailing hidden transfer word so an indirect
            // call through the vtable carries the same signature as every implementer.
            if (needsTransferWord()) {
                llvmTypes.push_back(
                    llvm::Type::getInt64Ty(*module->getLlvmContext()));
            }
            // Mirror the concrete path's return-by-pointer and sret rules too: the
            // indirect call type must match the implementer's, or a struct return
            // overflows a ptr-sized alloca and an sret slot displaces `this`.
            llvm::Type* llvmRetAbs;
            llvm::Type* sretStructTyAbs = nullptr;
            {
                CajetaTypePtr rt = returnType;
                bool isArrR = rt
                    && dynamic_pointer_cast<CajetaArray>(rt) != nullptr;
                auto rtClass = dynamic_pointer_cast<CajetaClass>(rt);
                bool isClassLikeR = rtClass != nullptr;
                bool isPrimR = rt && (rt->getTypeFlags() & PRIMITIVE_FLAG);
                bool isInterfaceR = rtClass && rtClass->isInterface();
                bool isValueTypeR = rt && rt->isValueType();
                bool sretReturnAbs = returnsStackValue();
                bool returnByPointer = isClassLikeR
                    && (isArrR || !isPrimR) && !isInterfaceR && !isValueTypeR;
                if (sretReturnAbs) {
                    sretStructTyAbs = rt ? rt->getLlvmType() : nullptr;
                    llvmRetAbs = llvm::Type::getVoidTy(
                        *module->getLlvmContext());
                    llvmTypes.insert(llvmTypes.begin(),
                        llvm::PointerType::get(
                            *module->getLlvmContext(), 0));
                } else if (returnByPointer) {
                    llvmRetAbs = llvm::PointerType::get(
                        *module->getLlvmContext(), 0);
                } else {
                    llvmRetAbs = rt ? rt->getLlvmType() : nullptr;
                }
                // Error recovery: a diagnosed return type recovered to
                // CajetaType::error() has no llvm type, and FunctionType::get(nullptr)
                // segfaults inside LLVM rather than diagnosing. void keeps the walk alive.
                if (!llvmRetAbs) {
                    llvmRetAbs = llvm::Type::getVoidTy(
                        *module->getLlvmContext());
                }
            }
            llvmFunctionType = llvmTypes.empty()
                ? llvm::FunctionType::get(llvmRetAbs, false)
                : llvm::FunctionType::get(llvmRetAbs, llvmTypes, false);
            return;
        }

        bool staticMethod = modifiers.find(STATIC) != modifiers.end();

        // A static multi-parameter function cannot return a borrow: no single
        // parameter's lifetime is implied. "Reference-typed" is decided by the cajeta
        // shape, not by isPointerTy — a class's LLVM type is its body struct.
        bool returnIsReferenceTyped = false;
        if (returnType) {
            if (auto rc = std::dynamic_pointer_cast<CajetaClass>(returnType)) {
                // Views and @ValueType classes are inline aggregates returned BY VALUE
                // (Copy), so they are owned results, not borrows, and are exempt.
                if (!std::dynamic_pointer_cast<CajetaView>(returnType)
                        && !returnType->isValueType()) {
                    returnIsReferenceTyped = true;
                }
            } else if (returnType->getLlvmType()
                    && returnType->getLlvmType()->isPointerTy()) {
                returnIsReferenceTyped = true;
            }
        }
        // An sret method is not borrow-returning either: the result is constructed
        // into the caller's slot by copy, so no parameter lifetime is inherited.
        if (staticMethod && !returnsOwnership && !returnsStackValue()
                && returnIsReferenceTyped
                && parameterList.size() > 1) {
            // Never fabricate the spelling: a type PARAMETER's QName comes back as the
            // wildcard sentinel `?`, so prescribing "#" + typeName invents a sigil.
            std::string retSpelling;
            if (returnType) {
                const std::string tn = returnType->getQName()
                    ? returnType->getQName()->getTypeName() : std::string();
                if (!tn.empty() && tn.find('?') == std::string::npos) {
                    retSpelling = tn;
                } else {
                    const std::string canon = returnType->toCanonical();
                    if (!canon.empty() && canon.find('?') == std::string::npos) {
                        retSpelling = canon;
                    }
                }
            }
            const std::string fix = retSpelling.empty()
                ? std::string("mark the return type `#` (for a type parameter "
                              "`T`, that is `#T`)")
                : ("use `#" + retSpelling + "` to return ownership");
            throw Exception(
                "multi-parameter free function '" + name + "' cannot return a "
                "borrow: with more than one parameter there is no single "
                "lifetime for the result to inherit, so the compiler cannot "
                "tell whose memory it points into. Fix: " + fix
                + ", or reduce to a single parameter",
                "CAJETA_ERROR_BORROW_RETURN_MULTI_PARAM");
        }

        if (!staticMethod && !thisAlreadyInserted) {
            auto thisParam = make_shared<FormalParameter>(string("this"), CajetaType::of("pointer"));
            thisParam->setParent(shared_from_this());
            parameterList.insert(parameterList.begin(), thisParam);
            parameters[thisParam->getName()] = thisParam;
        }

        for (auto formalParameter: parameterList) {
            CajetaTypePtr pt = formalParameter->getType();
            llvm::Type* ptLlvm = pt->getLlvmType();
            // Class instances, arrays AND structs pass BY POINTER: a struct is a typed
            // view over a buffer, so copying it at each boundary defeats its purpose.
            // CajetaArray carries PRIMITIVE_FLAG, so test the subtype, not the bit.
            bool isArr = dynamic_pointer_cast<CajetaArray>(pt) != nullptr;
            bool isClassLike = dynamic_pointer_cast<CajetaClass>(pt) != nullptr;
            bool isPrim = pt && (pt->getTypeFlags() & PRIMITIVE_FLAG);
            // @ValueType params pass BY VALUE (they are Copy): the call site loads the
            // inline slot and the signature carries getLlvmType().
            bool passByPointer = isClassLike && (isArr || !isPrim)
                && !pt->isValueType();
            if (passByPointer) {
                ptLlvm = llvm::PointerType::get(*module->getLlvmContext(), 0);
            }
            llvmTypes.push_back(ptLlvm);
        }
        // Trailing hidden i64 transfer word — bit i set iff user-arg i surrendered a
        // title. Trailing so every existing index (sret 0, `this`, user args) is
        // undisturbed, and outside parameterList so canonical/vtable keys are too.
        if (needsTransferWord()) {
            llvmTypes.push_back(llvm::Type::getInt64Ty(*module->getLlvmContext()));
        }
        // Returns follow the same by-pointer rule; an interface return travels as a
        // by-value fat-pointer body, and an sret return is `void` plus a hidden
        // leading `ptr` (before `this`) carrying the sret(structTy) attribute.
        bool sretReturn = returnsStackValue();
        llvm::Type* sretStructTy = nullptr;
        llvm::Type* llvmRet;
        {
            CajetaTypePtr rt = returnType;
            bool isArrR = rt
                && dynamic_pointer_cast<CajetaArray>(rt) != nullptr;
            auto rtClass = dynamic_pointer_cast<CajetaClass>(rt);
            bool isClassLikeR = rtClass != nullptr;
            bool isPrimR = rt && (rt->getTypeFlags() & PRIMITIVE_FLAG);
            bool isInterfaceR = rtClass && rtClass->isInterface();
            bool isValueTypeR = rt && rt->isValueType();
            bool returnByPointer = isClassLikeR && (isArrR || !isPrimR)
                && !isInterfaceR && !isValueTypeR;
            if (sretReturn) {
                sretStructTy = rt ? rt->getLlvmType() : nullptr;
                llvmRet = llvm::Type::getVoidTy(*module->getLlvmContext());
                llvmTypes.insert(llvmTypes.begin(),
                    llvm::PointerType::get(*module->getLlvmContext(), 0));
            } else if (returnByPointer) {
                llvmRet = llvm::PointerType::get(*module->getLlvmContext(), 0);
            } else {
                llvmRet = rt ? rt->getLlvmType() : nullptr;
            }
            if (!llvmRet) {
                llvmRet = llvm::Type::getVoidTy(*module->getLlvmContext());
            }
        }
        if (llvmTypes.size()) {
            llvmFunctionType = llvm::FunctionType::get(llvmRet, llvmTypes, false);
        } else {
            llvmFunctionType = llvm::FunctionType::get(llvmRet, false);
        }

        // Two-layer naming: instantiations of one canonical template get distinct
        // LLVM symbols from getLlvmSymbolName().
        string canonical = getLlvmSymbolName();
        // Reuse an existing Function: Function::Create would auto-rename (`name.1`)
        // and orphan the earlier one, which vtable globals already reference.
        if (llvm::Function* existing = module->getLlvmModule()->getFunction(canonical)) {
            if (existing->isDeclaration()
                    && existing->getFunctionType() != llvmFunctionType) {
                // A placeholder filled since this declaration, changing the ABI:
                // declare a fresh function, splice every existing use over and drop
                // the stale one. A function that already has a body is left alone.
                existing->setName(canonical + ".stale");
                llvm::Function* fresh = llvm::Function::Create(
                    llvmFunctionType, llvm::Function::ExternalLinkage,
                    canonical, module->getLlvmModule());
                existing->replaceAllUsesWith(fresh);
                existing->eraseFromParent();
                llvmFunction = fresh;
            } else {
                llvmFunction = existing;
            }
        } else {
            llvmFunction = llvm::Function::Create(llvmFunctionType, llvm::Function::ExternalLinkage,
                canonical, module->getLlvmModule());
        }

        // Force-inline @ValueType operators: AlwaysInlinerPass runs even at O0, so a
        // dispatched operator folds to flat, register-resident IR on host and device.
        if (parent && parent->isValueType()
                && name.rfind("operator", 0) == 0) {
            llvmFunction->addFnAttr(llvm::Attribute::AlwaysInline);
        }

        // @Inline / @NoInline — user-facing inlining control. @Inline forces the fold
        // even across ThinLTO module boundaries; @NoInline (+ cold) keeps a slow path
        // out of a hot method. Mutually exclusive: @Inline wins if both are present.
        if (findAnnotation("Inline") != nullptr) {
            llvmFunction->addFnAttr(llvm::Attribute::AlwaysInline);
        } else if (findAnnotation("NoInline") != nullptr) {
            llvmFunction->addFnAttr(llvm::Attribute::NoInline);
            llvmFunction->addFnAttr(llvm::Attribute::Cold);
        }

        if (sretReturn && sretStructTy && llvmFunction->arg_size() > 0) {
            llvmFunction->getArg(0)->addAttr(
                llvm::Attribute::getWithStructRetType(
                    *module->getLlvmContext(), sretStructTy));
        }

        archive[canonical] = shared_from_this();

        module->getLlvmModule()->getOrInsertFunction(canonical, llvmFunctionType);
    }

    // ---- Frame-arena escape analysis (frame-arena-plan U2) ----------------
    namespace {
        bool arenaIsStringType(const CajetaTypePtr& t) {
            if (!t || !t->getQName()) return false;
            return t->getQName()->getTypeName() == "String"
                && t->getQName()->getPackageName() == "cajeta.lang";
        }
        // Leftmost (receiver-side) identifier of a subtree — the name a `#move`
        // transfers, since `#c.next()` parses as `#(c.next())`.
        std::string arenaLeftmostId(const AbstractSyntaxNodePtr& node) {
            if (!node) return "";
            if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(node))
                return id->getTextValue();
            if (auto mc = std::dynamic_pointer_cast<MethodCallExpression>(node)) {
                if (!mc->getChildren().empty())
                    return arenaLeftmostId(mc->getChildren()[0]);
                return "";
            }
            for (auto& c : node->getChildren()) {
                auto r = arenaLeftmostId(c);
                if (!r.empty()) return r;
            }
            return "";
        }
        // The concat BinaryOpExpression directly initializing a declarator, if any.
        std::shared_ptr<BinaryOpExpression> arenaConcatInit(const VariableDeclaratorPtr& d) {
            if (!d || !d->getInitializer()) return nullptr;
            auto& kids = d->getInitializer()->getChildren();
            if (kids.empty()) return nullptr;
            auto bo = std::dynamic_pointer_cast<BinaryOpExpression>(kids[0]);
            if (bo && bo->getBinaryOp() == BINARY_OP_ADD) return bo;
            return nullptr;
        }
        // The `heap T[N]` NewExpression initializing a declarator, IF it is a
        // single-dimension array of PRIMITIVE elements: element drops are then no-ops,
        // so the arena reset reclaims it fully.
        std::shared_ptr<NewExpression> arenaArrayInit(const CajetaTypePtr& declType,
                                                      const VariableDeclaratorPtr& d) {
            // Use the declared local type (always populated post-resolve): the
            // NewExpression's resolvedType is not reliably set this early.
            auto arr = std::dynamic_pointer_cast<CajetaArray>(declType);
            if (!arr) return nullptr;
            if (!d || !d->getInitializer()) return nullptr;
            auto& kids = d->getInitializer()->getChildren();
            if (kids.empty()) return nullptr;
            auto ne = std::dynamic_pointer_cast<NewExpression>(kids[0]);
            if (!ne || ne->getStackAlloc() || ne->getSharedAlloc()) return nullptr;
            auto acr = std::dynamic_pointer_cast<ArrayCreatorRest>(ne->getCreatorRest());
            if (!acr || acr->getTotalBracketPairs() != 1) return nullptr;
            auto elem = arr->getElementType();
            if (!elem
                    || std::dynamic_pointer_cast<CajetaClass>(elem)
                    || std::dynamic_pointer_cast<CajetaArray>(elem)
                    || std::dynamic_pointer_cast<CajetaView>(elem)
                    || std::dynamic_pointer_cast<CajetaFunctionType>(elem)) {
                return nullptr;
            }
            return ne;
        }
        // Direct identifier RHS — an alias store. A name nested inside a call or an
        // operator is only borrowed, so only a bare top-level identifier counts.
        std::string arenaDirectIdentifier(const AbstractSyntaxNodePtr& node) {
            if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(node))
                return id->getTextValue();
            return "";
        }
        // Escape walk: collect the names that leave this frame (`#name`, returns,
        // stores, transferred call/ctor arguments) and the concat / array declarations
        // that are arena candidates.
        void arenaWalk(const AbstractSyntaxNodePtr& node,
                       std::set<std::string>& escaping,
                       std::vector<std::pair<std::string,
                           std::shared_ptr<BinaryOpExpression>>>& candidates,
                       std::vector<std::pair<std::string,
                           std::shared_ptr<NewExpression>>>& arrayCandidates) {
            if (!node) return;
            if (auto mv = std::dynamic_pointer_cast<MoveExpression>(node)) {
                if (!mv->getChildren().empty()) {
                    std::string n = arenaLeftmostId(mv->getChildren()[0]);
                    if (!n.empty()) escaping.insert(n);
                    for (auto& c : mv->getChildren()) arenaWalk(c, escaping, candidates, arrayCandidates);
                }
                return;
            }
            // A lambda's body hangs off `body`, not getChildren(), so the default walk
            // never sees it. Scan it for `#name` transfers INTO the closure, which may
            // outlive this frame; its own locals belong to a separate frame.
            if (auto lambda = std::dynamic_pointer_cast<LambdaExpression>(node)) {
                std::vector<std::pair<std::string,
                    std::shared_ptr<BinaryOpExpression>>> innerCands;
                std::vector<std::pair<std::string,
                    std::shared_ptr<NewExpression>>> innerArrays;
                arenaWalk(lambda->getBody(), escaping, innerCands, innerArrays);
                return;
            }
            // `return name` escapes; a returned concat builds a fresh value instead.
            if (auto ret = std::dynamic_pointer_cast<ReturnStatement>(node)) {
                std::string n = arenaDirectIdentifier(ret->getExpression());
                if (!n.empty()) escaping.insert(n);
                arenaWalk(ret->getExpression(), escaping, candidates, arrayCandidates);
                return;
            }
            if (auto th = std::dynamic_pointer_cast<ThrowStatement>(node)) {
                arenaWalk(th->getExpression(), escaping, candidates, arrayCandidates);
                return;
            }
            if (auto bo = std::dynamic_pointer_cast<BinaryOpExpression>(node)) {
                if (bo->getBinaryOp() == BINARY_OP_ASSIGN
                        && bo->getChildren().size() >= 2) {
                    std::string n = arenaDirectIdentifier(bo->getChildren()[1]);
                    if (!n.empty()) escaping.insert(n);
                }
                for (auto& c : bo->getChildren()) arenaWalk(c, escaping, candidates, arrayCandidates);
                return;
            }
            if (auto lvd = std::dynamic_pointer_cast<LocalVariableDeclaration>(node)) {
                bool isStr = arenaIsStringType(lvd->getType());
                for (auto& d : lvd->getVariableDeclarators()) {
                    if (!d) continue;
                    if (d->getInitializer()) {
                        auto& kids = d->getInitializer()->getChildren();
                        if (!kids.empty()) {
                            std::string alias = arenaDirectIdentifier(kids[0]);
                            if (!alias.empty()) escaping.insert(alias);
                        }
                        arenaWalk(d->getInitializer(), escaping, candidates, arrayCandidates);
                    }
                    if (isStr) {
                        if (auto cbo = arenaConcatInit(d))
                            candidates.emplace_back(d->getIdentifier(), cbo);
                    }
                    if (auto ane = arenaArrayInit(lvd->getType(), d))
                        arrayCandidates.emplace_back(d->getIdentifier(), ane);
                }
                return;
            }
            if (auto mc = std::dynamic_pointer_cast<MethodCallExpression>(node)) {
                // A `#arg` is recorded as callerTransferred: the name leaves the frame.
                for (auto& p : mc->getParameters()) {
                    if (p.callerTransferred) {
                        std::string n = arenaLeftmostId(p.expression);
                        if (!n.empty()) escaping.insert(n);
                    }
                }
                node->forEachSubNode([&](const AbstractSyntaxNodePtr& sub) {
                    arenaWalk(sub, escaping, candidates, arrayCandidates);
                });
                return;
            }
            if (auto ne = std::dynamic_pointer_cast<NewExpression>(node)) {
                // Constructor arguments live in the CreatorRest's parameter list, NOT
                // getChildren(). A `#arg` there transfers INTO the new object, so
                // missing it arena-routes a buffer the object still points at.
                if (auto cr = std::dynamic_pointer_cast<ClassCreatorRest>(
                        ne->getCreatorRest())) {
                    for (auto& p : cr->getParameters()) {
                        if (p.callerTransferred) {
                            std::string n = arenaLeftmostId(p.expression);
                            if (!n.empty()) escaping.insert(n);
                        }
                    }
                }
                node->forEachSubNode([&](const AbstractSyntaxNodePtr& sub) {
                    arenaWalk(sub, escaping, candidates, arrayCandidates);
                });
                return;
            }
            node->forEachSubNode([&](const AbstractSyntaxNodePtr& sub) {
                arenaWalk(sub, escaping, candidates, arrayCandidates);
            });
        }
    }

    bool Method::retainsFormal(const std::string& formalName) {
        auto cached = retainsFormalCache.find(formalName);
        if (cached != retainsFormalCache.end()) return cached->second;
        bool retained = false;
        std::function<void(const AbstractSyntaxNodePtr&)> walk =
            [&](const AbstractSyntaxNodePtr& node) {
                if (!node || retained) return;
                if (auto bin = std::dynamic_pointer_cast<BinaryOpExpression>(node)) {
                    auto& kids = bin->getChildren();
                    if (kids.size() >= 2) {
                        auto lhs = kids[0];
                        auto rhs = kids[1];
                        bool lhsIsPlace =
                            std::dynamic_pointer_cast<DotExpression>(lhs)
                            || std::dynamic_pointer_cast<ArrayIndexExpression>(lhs);
                        auto rhsInner = rhs;
                        if (auto mv = std::dynamic_pointer_cast<MoveExpression>(rhs)) {
                            auto& mk = mv->getChildren();
                            if (!mk.empty()) rhsInner = mk[0];
                        }
                        auto rhsId =
                            std::dynamic_pointer_cast<IdentifierExpression>(rhsInner);
                        if (lhsIsPlace && rhsId
                                && rhsId->getTextValue() == formalName) {
                            retained = true;
                            return;
                        }
                    }
                }
                // Descend through forEachSubNode: statements and calls keep their
                // payloads in private fields, which getChildren() silently misses.
                node->forEachSubNode(walk);
            };
        walk(block);
        retainsFormalCache[formalName] = retained;
        return retained;
    }

    // Last-use pre-pass: record the latest (line, column) each identifier is read at,
    // parking any name read inside a loop body — its textual last use runs again.
    void Method::computeLastUses() {
        if (lastUsesComputed) return;
        lastUsesComputed = true;
        if (!block) return;
        std::function<void(const AbstractSyntaxNodePtr&, bool)> walk =
            [&](const AbstractSyntaxNodePtr& node, bool inLoop) {
                if (!node) return;
                bool loopHere = inLoop
                    || std::dynamic_pointer_cast<WhileStatement>(node)
                    || std::dynamic_pointer_cast<DoStatement>(node)
                    || std::dynamic_pointer_cast<ForStatement>(node)
                    || std::dynamic_pointer_cast<EnhancedForStatement>(node);
                if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(node)) {
                    const std::string& n = id->getTextValue();
                    if (loopHere) {
                        loopUsedNames.insert(n);
                    }
                    pair<int, int> at{id->getSourceLine(), id->getSourceColumn()};
                    auto it = lastUseAt.find(n);
                    if (it == lastUseAt.end() || at > it->second) {
                        lastUseAt[n] = at;
                    }
                }
                node->forEachSubNode([&](const AbstractSyntaxNodePtr& sub) {
                    walk(sub, loopHere);
                });
            };
        walk(block, false);
    }

    bool Method::isFinalUseOfLocal(const std::string& name, int line, int column) {
        computeLastUses();
        if (loopUsedNames.find(name) != loopUsedNames.end()) return false;
        auto it = lastUseAt.find(name);
        if (it == lastUseAt.end()) return false;
        return it->second == pair<int, int>{line, column};
    }

    static std::atomic<int64_t> g_bodyResolveWalks{0};

    int64_t Method::bodyResolveWalks() {
        return g_bodyResolveWalks.load(std::memory_order_relaxed);
    }

    void Method::resolveBody(CajetaModulePtr module) {
        if (bodyResolved) return;
        if (!block) { bodyResolved = true; return; }
        // Marked BEFORE the walk: a body that throws has still been attempted, and
        // re-walking it later would re-raise the same error from another phase.
        bodyResolved = true;
        g_bodyResolveWalks.fetch_add(1, std::memory_order_relaxed);
        // Semantic errors escaping a SCRIPT module's body are rewritten into host
        // coordinates here; remapScriptException no-ops for ordinary modules.
        try {
            block->resolveTypes(module);
        } catch (cajeta::Exception& e) {
            remapScriptException(module, e);
            throw;
        }
    }

    void Method::resolveBodyForLint(CajetaModulePtr module) {
        if (bodyResolved || !block || !module) return;

        const bool pushedClass = (parent != nullptr);
        if (pushedClass) module->getStructureStack().push_back(parent);
        module->getScopeStack().add(make_shared<Scope>(toCanonical(), module));
        module->getScopeStack().peek()->setParent(nullptr);

        for (auto& parameter : parameterList) {
            if (!parameter) continue;
            // StackField: name + declared type only, no alloca — resolution reads
            // getType() and nothing else.
            auto field = make_shared<StackField>(module, parameter->getName(),
                                                 parameter->getType());
            module->getScopeStack().peek()->putField(field);
        }

        // The CALLER half of every call edge this body records: noteResolvedCallXref
        // reads it from the module, and a resolve-only walk would leave edges caller-less.
        MethodPtr priorMethod = module->getCurrentMethod();
        module->setCurrentMethod(
            static_pointer_cast<Method>(shared_from_this()));

        const bool priorMode = module->isResolutionOnly();
        module->setResolutionOnly(true);
        try {
            resolveBody(module);
        } catch (...) {
            module->setResolutionOnly(priorMode);
            module->setCurrentMethod(priorMethod);
            module->getScopeStack().pop();
            if (pushedClass) module->getStructureStack().pop_back();
            throw;
        }
        module->setResolutionOnly(priorMode);
        module->setCurrentMethod(priorMethod);
        module->getScopeStack().pop();
        if (pushedClass) module->getStructureStack().pop_back();
    }

    void Method::computeArenaEligibility() {
        arenaEligibleNames.clear();
        methodUsesArena = false;
        if (!block) return;
        std::set<std::string> escaping;
        std::vector<std::pair<std::string, std::shared_ptr<BinaryOpExpression>>> candidates;
        std::vector<std::pair<std::string, std::shared_ptr<NewExpression>>> arrayCandidates;
        arenaWalk(block, escaping, candidates, arrayCandidates);
        for (auto& cand : candidates) {
            // Eligible iff the name never escapes; a name declared more than once is
            // eligible only when NO declaration escapes (conservative on shadowing).
            if (escaping.find(cand.first) != escaping.end()) continue;
            cand.second->setArenaEligible(true);
            arenaEligibleNames.insert(cand.first);
            methodUsesArena = true;
        }
        for (auto& cand : arrayCandidates) {
            if (escaping.find(cand.first) != escaping.end()) continue;
            cand.second->setArenaEligible(true);
            arenaEligibleNames.insert(cand.first);
            methodUsesArena = true;
        }
        // `stack [1,2,3]` literals take the same non-escape gate — escape is tracked
        // by name, not by initializer kind. The rest fall back to the heap path.
        auto arenaPrimitiveElem = [](const CajetaTypePtr& elem) -> bool {
            return elem
                && !std::dynamic_pointer_cast<CajetaClass>(elem)
                && !std::dynamic_pointer_cast<CajetaArray>(elem)
                && !std::dynamic_pointer_cast<CajetaView>(elem)
                && !std::dynamic_pointer_cast<CajetaFunctionType>(elem);
        };
        std::vector<std::pair<std::string,
            std::shared_ptr<ArrayLiteralExpression>>> literalCandidates;
        std::function<void(const AbstractSyntaxNodePtr&)> collectStackLiterals =
            [&](const AbstractSyntaxNodePtr& node) {
                if (!node) return;
                if (auto lvd =
                        std::dynamic_pointer_cast<LocalVariableDeclaration>(node)) {
                    auto arr = std::dynamic_pointer_cast<CajetaArray>(lvd->getType());
                    if (arr && arenaPrimitiveElem(arr->getElementType())) {
                        for (auto& d : lvd->getVariableDeclarators()) {
                            if (!d || !d->getInitializer()) continue;
                            auto& kids = d->getInitializer()->getChildren();
                            if (kids.empty()) continue;
                            if (auto lit = std::dynamic_pointer_cast<
                                    ArrayLiteralExpression>(kids[0])) {
                                if (lit->isStackAlloc())
                                    literalCandidates.emplace_back(
                                        d->getIdentifier(), lit);
                            }
                        }
                    }
                }
                node->forEachSubNode(collectStackLiterals);
            };
        collectStackLiterals(block);
        for (auto& cand : literalCandidates) {
            if (escaping.find(cand.first) != escaping.end()) continue;
            cand.second->setArenaEligible(true);
            arenaEligibleNames.insert(cand.first);
            methodUsesArena = true;
        }
    }

    // Lower this method's body into its LLVM function: prologue (scope frame,
    // formals, drop entries, debug/profile frames), advice wrapping, the constructor
    // preamble, the body itself, then the exit drops and terminator.
    void Method::generateCode() {
        ensureFreshPrototype();
        auto& llvmFunction = llvmFunctionRef();
        auto& llvmFunctionType = llvmFunctionTypeRef();
        auto& llvmOriginalFunction = llvmOriginalFunctionRef();
        if (getenv("CAJETA_DBG_GENCODE")) {
            std::cerr << "[gencode] "
                << (parent && parent->getQName()
                        ? parent->getQName()->toCanonical() : "?")
                << "::" << name
                << " frozen=" << (isFrozen() ? 1 : 0)
                << " fn=" << (void*) llvmFunction
                << " cachedTy=" << (void*) llvmFunctionType
                << " cachedN=" << (llvmFunctionType
                        ? (int) llvmFunctionType->getNumParams() : -1)
                << " fnTy=" << (llvmFunction
                        ? (void*) llvmFunction->getFunctionType() : nullptr)
                << " fnN=" << (llvmFunction
                        ? (int) llvmFunction->getFunctionType()->getNumParams()
                        : -1)
                << " formals=" << parameterList.size() << std::endl;
        }
        // Emit-target swap (test-reuse): `module` is handed down to every statement,
        // so this one swap redirects all emission into the user emit module. The
        // save/restore below is what keeps a NESTED body from clobbering its caller.
        CajetaModulePtr savedModule = module;
        { CajetaModulePtr em = getEmitModule();
          if (em && em != module) module = em; }
        struct RestoreModule {
            Method* self; CajetaModulePtr saved;
            ~RestoreModule() { self->module = saved; }
        } restoreModule{this, savedModule};

        // Mark this module as the active codegen frame, so a cross-module template
        // instantiation triggered under it is attributed correctly. RAII; nests.
        struct CodegenFrame {
            CajetaModulePtr prev;
            explicit CodegenFrame(const CajetaModulePtr& m)
                : prev(CajetaModule::getCurrentCodegenModule()) {
                CajetaModule::setCurrentCodegenModule(m);
            }
            ~CodegenFrame() { CajetaModule::setCurrentCodegenModule(prev); }
        } codegenFrame(module);

        // The lints fire before the abstract / method-template early-returns: the body
        // is still walkable there, and each warning dedupes itself per declaration.
        lintHeapOptionalReturn();
        lintPlainReturnYieldsTitle();
        lintPlainReturnOfOwnedSlot();
        if (abstractFlag) return;
        // A method template is emitted per instantiation, never as a declaration.
        if (isMethodTemplate()) return;
        if (llvmBasicBlock != nullptr) {
            return;
        }

        // @Kernel parameter-type validation (no-op otherwise): throws XPU-K01 before
        // codegen. The llvmBasicBlock guard above makes this once per method.
        cajeta::xpu::validateKernelParams(shared_from_this());

        // Re-establish {T -> arg} for the body (the instantiation walk already popped
        // its frame), and detach the per-FUNCTION control-flow stacks: a nested body
        // that saw the caller's open try frames would pop the caller's live frame.
        struct FunctionStacksGuard {
            CajetaModulePtr mod;
            CajetaModule::FunctionCodegenStacks saved;
            explicit FunctionStacksGuard(CajetaModulePtr m)
                : mod(std::move(m)), saved(mod->takeFunctionCodegenStacks()) {}
            ~FunctionStacksGuard() {
                mod->restoreFunctionCodegenStacks(std::move(saved));
            }
        } functionStacksGuard{module};

        struct SubstFrameGuard {
            CajetaModulePtr mod; bool pushed = false;
            ~SubstFrameGuard() { if (pushed) mod->popTypeSubstitution(); }
        } substGuard{module};
        if (parent && parent->isInstantiation()) {
            const auto& tparams = parent->getTypeParameters();
            const auto& targs = parent->getTypeArguments();
            if (!tparams.empty() && tparams.size() == targs.size()) {
                std::map<std::string, CajetaTypePtr> frame;
                if (auto inherited = module->getCurrentTypeSubstitution()) {
                    frame = *inherited;
                }
                for (size_t i = 0; i < tparams.size(); ++i) {
                    frame[tparams[i].name] = targs[i];
                }
                module->pushTypeSubstitution(std::move(frame));
                substGuard.pushed = true;
            }
        }

        // A @Kernel or @Device method taking Buffer<T> works on device memory and is
        // never called on the host — its real lowering is the device function. Emit a
        // trivial host stub so host codegen never lowers device-only constructs.
        if (cajeta::xpu::isKernel(*this) || cajeta::xpu::isDevice(*this)) {
            bool hasDeviceBuffer = false;
            for (auto& p : parameterList) {
                if (p && p->getType()
                        && p->getType()->toCanonical().rfind(
                               "cajeta.xpu.KernelBuffer", 0) == 0) {
                    hasDeviceBuffer = true;
                    break;
                }
            }
            if (hasDeviceBuffer) {
                llvm::BasicBlock* bb = llvm::BasicBlock::Create(
                    *module->getLlvmContext(), "entry", llvmFunction);
                llvm::IRBuilder<> b(bb);
                if (llvmFunction->getReturnType()->isVoidTy()) {
                    b.CreateRetVoid();
                } else {
                    b.CreateRet(llvm::Constant::getNullValue(
                        llvmFunction->getReturnType()));
                }
                return;
            }
        }

        // A bounded-wildcard instantiation is an abstract handle: calls are
        // PECS-rejected at the site and there is no concrete element layout to lower
        // against. Emit a trap stub so the vtable/method symbol still resolves.
        if (parent && parent->isBoundedWildcardInstantiation() && llvmFunction) {
            llvm::BasicBlock* bb = llvm::BasicBlock::Create(
                *module->getLlvmContext(), "entry", llvmFunction);
            llvm::IRBuilder<> b(bb);
            if (llvmFunction->getReturnType()->isVoidTy()) {
                b.CreateRetVoid();
            } else {
                b.CreateRet(llvm::Constant::getNullValue(
                    llvmFunction->getReturnType()));
            }
            llvmBasicBlock = bb;   // mark emitted (idempotency guard above)
            return;
        }

        // @Native("symbol") — the body is a forwarding call to a C runtime symbol,
        // declared extern here and resolved at link or JIT time.
        if (auto nativeAnn = findAnnotation("Native")) {
            // Two forms: @Native("__cajeta_x") for a runtime symbol, and
            // @Native(symbol=..., lib=...) which additionally records a native
            // requirement for the resolver. Codegen is identical for both.
            std::string symbol = nativeAnn->getString("symbol");
            if (symbol.empty()) symbol = nativeAnn->getString("value");
            std::string lib = nativeAnn->getString("lib");
            if (symbol.empty()) {
                cerr << "@Native on " << buildCanonical(parent, name, parameterList, true)
                     << " requires a symbol-name argument" << std::endl;
                return;
            }
            emitNativeForwardingBody(symbol);
            if (!lib.empty()) recordNativeRequirement(lib, symbol);
            return;
        }
        // @Around: the user body goes into llvmOriginalFunction so llvmFunction can
        // host the wrapper. It cannot move to generatePrototype — matchingAdvice is
        // populated after parse, and prototypes are built during it.
        if (!llvmOriginalFunction) {
            for (auto& m : matchingAdvice) {
                if (m.kind == AdviceKind::Around) {
                    std::string originalName = getLlvmSymbolName() + "__original";
                    llvmOriginalFunction = llvm::Function::Create(
                        llvmFunctionType, llvm::Function::ExternalLinkage,
                        originalName, getEmitModule()->getLlvmModule());
                    break;
                }
            }
        }
        llvm::Function* bodyFn = llvmOriginalFunction
            ? llvmOriginalFunction : llvmFunction;
        bool hasAroundWrapper = (llvmOriginalFunction != nullptr);

        llvmBasicBlock = llvm::BasicBlock::Create(*module->getLlvmContext(), "entry", bodyFn);
        builder = new llvm::IRBuilder<>(llvmBasicBlock, llvmBasicBlock->begin());
        builder->SetInsertPoint(llvmBasicBlock);
        // Save and restore the module's cursor scalars: a body emitted on-reference
        // while NESTED inside another body shares them, so the outer method's insert
        // point and current-method must survive this call.
        llvm::IRBuilder<>* prevBuilder = module->getBuilder();
        MethodPtr prevCurrentMethod = module->getCurrentMethod();
        struct RestoreCursor {
            CajetaModulePtr m; llvm::IRBuilder<>* b; MethodPtr cm;
            ~RestoreCursor() { m->setBuilder(b); m->setCurrentMethod(cm); }
        } restoreCursor{module, prevBuilder, prevCurrentMethod};
        module->setBuilder(builder);
        module->setCurrentMethod(shared_from_this());
        // Mark the llvm module new IR lands in for this body (bodyFn's parent):
        // runtime externs, string constants, trampolines and lambda fns all read it.
        llvm::Module* prevEmitLlvmModule = CajetaModule::getCurrentEmitLlvmModule();
        CajetaModule::setCurrentEmitLlvmModule(bodyFn->getParent());
        struct RestoreEmitLlvm {
            llvm::Module* prev;
            ~RestoreEmitLlvm() { CajetaModule::setCurrentEmitLlvmModule(prev); }
        } restoreEmitLlvm{prevEmitLlvmModule};

        // Push the enclosing class so bare method/field references in the body resolve
        // against it — the parse-time push is long gone by codegen.
        bool pushedClass = false;
        if (parent) {
            module->getStructureStack().push_back(parent);
            pushedClass = true;
        }

        createScope();

        // Value-returning (sret) methods reserve arg 0 for the hidden result
        // pointer, so the real parameters (including `this`) start at arg 1.
        int i = returnsStackValue() ? 1 : 0;
        for (auto& parameter: parameterList) {
            FieldPtr parameterField = make_shared<ParameterField>(module, parameter, bodyFn, i++);
            module->getScopeStack().peek()->putField(parameterField);
        }
        // The hidden transfer word rides as the LAST llvm arg; stash it for the
        // callee-side consumers (the runtime-owner formal entries).
        if (needsTransferWord() && bodyFn->arg_size() > 0) {
            setTransferWordArg(bodyFn->getArg(bodyFn->arg_size() - 1));
        } else {
            setTransferWordArg(nullptr);
        }
        emitFormalDropEntries(module);

        // A closure-specialized instance had its function-typed parameters dropped
        // from the signature: bind each name so a call lowers to a DIRECT call.
        for (auto& bc : boundClosures) {
            auto boundField = make_shared<BoundClosureField>(
                module, bc.name, bc.fnType, bc.fn, bc.record);
            module->getScopeStack().peek()->putField(boundField);
        }

        // A session compile seeds the entry's root scope with earlier units' bindings.
        seedSessionScope(module);

        // Capture the caller-side scope_top in an alloca, then push the function-body
        // frame: every return path calls __cajeta_scope_exit_to(watermark), so nested
        // `scope { }` frames are waited and popped however the function exits.
        auto& ctx = *module->getLlvmContext();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        // Emit the frame ONLY when the body spawns or scopes; otherwise scopeWatermark
        // stays null and every exit_to site is guarded on it.
        bool needsScopeFrame = bodyNeedsScopeFrame();
        if (needsScopeFrame) {
            scopeWatermark = builder->CreateAlloca(ptrTy, nullptr, "scope_watermark");
            if (llvm::Function* saveFn = module->getRuntimeFunction(
                    "__cajeta_scope_save_top")) {
                llvm::Value* mark = builder->CreateCall(saveFn, {});
                builder->CreateStore(mark, scopeWatermark);
            }
            // --lazy-scope pushes the frame at the spawn site instead; off by default.
            if (!module->getFlags().lazyScope) {
                if (llvm::Function* enterFn = module->getRuntimeFunction(
                        "__cajeta_scope_enter")) {
                    builder->CreateCall(enterFn, {});
                }
            }
        }

        methodArenaMark = nullptr;

        // Push this method's debug frame (no-op unless --debug-info), paired with
        // __cajeta_dbg_frame_leave on every return path. The node lands in a slot, so
        // a leave unlinks exactly this invocation's frame.
        dbgFrameSlot = nullptr;
        lineFrameEmitted = false;
        profFrame = prof::ProfileFrame{};
        if (llvm::Value* dbgNode =
                dbg::emitDbgFrameEnter(module, getLlvmSymbolName())) {
            llvm::IRBuilder<>* b = module->getBuilder();
            dbgFrameSlot = b->CreateAlloca(
                llvm::PointerType::get(b->getContext(), 0), nullptr,
                "__dbg_frame_slot");
            b->CreateStore(dbgNode, dbgFrameSlot);
        }

        // Push a line-info shadow frame carrying this method's type/method/file (no-op
        // unless --line-info); every return path pairs it with __cajeta_line_leave.
        {
            std::string typeName = (parent && parent->getQName())
                ? parent->getQName()->toCanonical() : std::string();
            // The DECLARING class's file, not the module's: the stdlib parses every
            // file into one module whose source path is empty. Both forms are remapped
            // so frame descriptors stay byte-identical across build roots.
            std::string fileName = parent ? parent->getDeclaringFile()
                                          : std::string();
            if (fileName.empty()) fileName = module->remappedSourcePath();
            // The synthesized script wrapper is invisible in traces: its frames render
            // as `<script>`, and the file is the host's name for the unit.
            std::string frameMethod = getName();
            if (parent && parent->isScriptSynthesized()) {
                typeName = "<script>";
                fileName = module->scriptDiagFile();
                if (frameMethod == scriptEntryName()) {
                    frameMethod = "<script>";
                }
            }
            dbg::emitLineEnter(module, typeName, frameMethod, fileName);
            // Record that this method HAS a shadow frame, so a leave site can tell it
            // from an inline-codegen'd lambda body that never pushed one.
            lineFrameEmitted = true;

            // The exact-count / exact-time probe pair, carrying the same names as the
            // shadow frame. No-op unless --profiler=instrument selects this class.
            profFrame = prof::emitProfileEnter(module, typeName, frameMethod,
                                               fileName);
        }

        // Register the parameters as debug-frame locals. Materializing their slots at
        // entry makes every parameter inspectable from the first statement on.
        if (module->getFlags().debugInfo) {
            for (auto& parameter : parameterList) {
                FieldPtr pf = module->getScopeStack().peek()
                                    ->getField(parameter->getName());
                if (!pf || !pf->getType()) continue;
                // Memory facets: a primitive lives inline in the slot (Stack), while a
                // class/array/view parameter holds a pointer (Heap).
                CajetaTypePtr pt = pf->getType();
                bool isArr  = dynamic_pointer_cast<CajetaArray>(pt) != nullptr;
                bool isPrim = (pt->getTypeFlags() & PRIMITIVE_FLAG) && !isArr;
                dbg::FieldFacetInputs facetIn;
                facetIn.isStackField = isPrim;
                facetIn.isHeapField  = !isPrim;
                // A parameter's STATIC ownership role is its calling form alone: a
                // borrow now also carries a GATED drop entry, so keying off the entry
                // would misclassify every borrow as an Owner.
                facetIn.ownsDrop     = parameter->isTransferred();
                facetIn.isReference  = !isPrim && !parameter->isTransferred();
                // `this` is typed `pointer`, which the inspector cannot decode, so
                // register the OWNING class instead. Reference classes only: a
                // value-type `this` points at the value and would misread the slot.
                std::string dbgType = pt->toCanonical();
                if (parameter->getName() == "this" && parent
                        && parent->getQName() && !parent->isValueType()) {
                    dbgType = parent->getQName()->toCanonical();
                }
                dbg::emitDbgLocal(module, pf->getName(),
                                  dbgType,
                                  pf->getOrCreateAllocation(),
                                  dbg::classifyField(facetIn),
                                  pf->getDropEntry());
            }
        }

        // @NonNull checks fire BEFORE the try frame, so an argument-contract violation
        // escapes any in-method @AfterThrowing handling.
        emitNonNullParamChecks(module);

        // With @AfterThrowing matched (and no @Around wrapper, which owns its own
        // frame), wrap the body in a try frame: setjmp here, body in tryBB, catchBB
        // fires @AfterThrowing + @After and re-raises.
        TryFrameInfo bodyTryFrame{nullptr, nullptr, nullptr};
        bool wrapBodyForAfterThrowing =
            !hasAroundWrapper && hasAfterThrowingAdvice();
        if (wrapBodyForAfterThrowing) {
            bodyTryFrame = emitAfterThrowingTryEntry(
                module, *builder, bodyFn);
        }

        // @Before fires right after scope_enter, inside the try frame, so it sees the
        // scope the body will populate — but with an @Around wrapper it fires there
        // instead (the flatten rule for multiple aspects on one method).
        if (!hasAroundWrapper) emitBeforeAdvice(module);

        // vbase init: before any inherited-field access and before the super-ctor
        // calls, point self's vbase slots at the inline ancestor sub-objects. A
        // diamond descendant re-points non-first parents' slots further below.
        if (constructor && parent && bodyFn->arg_size() > 0
                && !parent->getVbaseAncestors().empty()) {
            llvm::Value* receiver = bodyFn->getArg(0);
            auto& vctx = *module->getLlvmContext();
            llvm::Type* vi8Ty = llvm::Type::getInt8Ty(vctx);
            llvm::Type* vi64Ty = llvm::Type::getInt64Ty(vctx);
            llvm::Type* parentLlvmType = parent->getLlvmType();
            for (auto& anc : parent->getVbaseAncestors()) {
                if (!anc) continue;
                int slotIdx = parent->getVbaseSlotIndex(anc.get());
                if (slotIdx < 0) continue;
                llvm::Value* slotPtr = builder->CreateStructGEP(
                    parentLlvmType, receiver, (unsigned) slotIdx,
                    "vbase_init_slot");
                uint64_t off = parent->getSubObjectByteOffset(anc.get());
                llvm::Value* ancPtr = (off == 0)
                    ? receiver
                    : builder->CreateInBoundsGEP(vi8Ty, receiver,
                        llvm::ConstantInt::get(vi64Ty, off),
                        "vbase_init_target");
                builder->CreateStore(ancPtr, slotPtr);
            }
        }

        // Also initialize each first-parent-chain ancestor's OWN vbase slots: an
        // upcast inherited-field read loads the BASE sub-object's slot, which stays
        // null when that ancestor's ctor never runs. They all sit at byte offset 0.
        if (constructor && parent && bodyFn->arg_size() > 0) {
            llvm::Value* receiver = bodyFn->getArg(0);
            auto& vctx = *module->getLlvmContext();
            llvm::Type* vi8Ty = llvm::Type::getInt8Ty(vctx);
            llvm::Type* vi64Ty = llvm::Type::getInt64Ty(vctx);
            CajetaClass* chain = parent->getSuperClasses().empty()
                ? nullptr : parent->getSuperClasses().front().get();
            while (chain) {
                if (!chain->getVbaseAncestors().empty()) {
                    llvm::Type* chainLlvm = chain->getLlvmType();
                    for (auto& anc : chain->getVbaseAncestors()) {
                        if (!anc) continue;
                        int slotIdx = chain->getVbaseSlotIndex(anc.get());
                        if (slotIdx < 0) continue;
                        llvm::Value* slotPtr = builder->CreateStructGEP(
                            chainLlvm, receiver, (unsigned) slotIdx,
                            "vbase_init_inh_slot");
                        uint64_t off = parent->getSubObjectByteOffset(anc.get());
                        llvm::Value* ancPtr = (off == 0)
                            ? receiver
                            : builder->CreateInBoundsGEP(vi8Ty, receiver,
                                llvm::ConstantInt::get(vi64Ty, off),
                                "vbase_init_inh_target");
                        builder->CreateStore(ancPtr, slotPtr);
                    }
                }
                chain = chain->getSuperClasses().empty()
                    ? nullptr : chain->getSuperClasses().front().get();
            }
        }

        if (constructor && parent && bodyFn->arg_size() > 0) {
            // Pre-walk the body for an explicit `super(args)`: the warning below only
            // applies when the user actually picked one parent's constructor.
            bool userHasExplicitSuperCtor = false;
            if (block) {
                // ExpressionStatement and ReturnStatement hold their expression in a
                // member field, not in `children` — forward into it explicitly.
                std::function<bool(AbstractSyntaxNodePtr)> findSuperCtor =
                    [&](AbstractSyntaxNodePtr node) -> bool {
                        if (!node) return false;
                        if (auto mce = std::dynamic_pointer_cast<
                                cajeta::MethodCallExpression>(node)) {
                            if (mce->isSuperCtorCall()) return true;
                        }
                        if (auto es = std::dynamic_pointer_cast<
                                cajeta::ExpressionStatement>(node)) {
                            if (findSuperCtor(es->getExpression())) return true;
                        }
                        for (auto& child : node->getChildren()) {
                            if (findSuperCtor(child)) return true;
                        }
                        return false;
                    };
                userHasExplicitSuperCtor = findSuperCtor(block);
            }
            llvm::Value* receiver = bodyFn->getArg(0);
            int parentIdx = 0;
            for (auto& sup : parent->getSuperClasses()) {
                bool isNonFirstParent = (parentIdx > 0);
                parentIdx++;
                if (!sup) continue;
                std::vector<ParameterEntry> noArgs;
                std::string supCtorName = sup->getQName()->getTypeName();
                // Warn only when all three hold: the user wrote an explicit super(...),
                // this is a sibling parent, and it has BOTH a no-arg and an args ctor.
                if (userHasExplicitSuperCtor && isNonFirstParent) {
                    bool supHasNoArg = false;
                    bool supHasArgs = false;
                    // Walk `methods` (the map by canonical), not `methodList`:
                    // constructors are kept in the map only.
                    for (auto& [name, m] : sup->getMethods()) {
                        if (!m || !m->isConstructor()) continue;
                        if (m->getModifiers().find(STATIC)
                                != m->getModifiers().end()) continue;
                        // parameterList includes the implicit `this`.
                        int userArgs = (int) m->getParameterList().size() - 1;
                        if (userArgs <= 0) supHasNoArg = true;
                        else supHasArgs = true;
                    }
                    if (supHasNoArg && supHasArgs) {
                        std::ostringstream w;
                        w << "warning: [implicit-ctor-skip] in "
                            << parent->getQName()->toCanonical()
                            << "(): explicit super(...) targets only the "
                            << "first parent's ctor; sibling parent '"
                            << sup->getQName()->toCanonical()
                            << "' also has an args constructor — its "
                            << "no-arg constructor was picked implicitly. "
                            << "Consider super<"
                            << sup->getQName()->getTypeName()
                            << ">(...) once that grammar lands, or "
                            << "restructure to pick explicitly via "
                            << "composition.\n";
                        logLine("warn", w.str());
                    }
                }
                if (sup->resolveMethod(supCtorName, noArgs,
                        /*isConstructor=*/true, /*floatingParams=*/false)) {
                    // Per-parent sub-object adjustment: the parent ctor was compiled
                    // against the parent's standalone layout, so pass a pointer to
                    // where its sub-object lives inside this instance.
                    llvm::Value* supThis = receiver;
                    uint64_t off = parent->getSubObjectByteOffset(sup.get());
                    if (off != 0) {
                        llvm::Type* i8Ty = llvm::Type::getInt8Ty(
                            *module->getLlvmContext());
                        supThis = builder->CreateInBoundsGEP(i8Ty, receiver,
                            llvm::ConstantInt::get(
                                llvm::Type::getInt64Ty(*module->getLlvmContext()),
                                off),
                            "super_ctor_subobj");
                    }
                    sup->invokeMethod(supCtorName, noArgs,
                        /*isConstructor=*/true,
                        supThis,
                        /*callerModule=*/module);
                }
            }
        }

        // Diamond fixup, after every parent ctor has run: re-point each NON-FIRST
        // parent's vbase slots at self's canonical sub-object positions, which is what
        // makes a diamond ancestor shared. The first parent already points there.
        if (constructor && parent && bodyFn->arg_size() > 0
                && parent->getSuperClasses().size() > 1) {
            llvm::Value* receiver = bodyFn->getArg(0);
            auto& fctx = *module->getLlvmContext();
            llvm::Type* fi8Ty = llvm::Type::getInt8Ty(fctx);
            llvm::Type* fi64Ty = llvm::Type::getInt64Ty(fctx);
            int fpIdx = 0;
            for (auto& sup : parent->getSuperClasses()) {
                bool isNonFirst = (fpIdx > 0);
                fpIdx++;
                if (!isNonFirst) continue;
                if (!sup) continue;
                if (sup->getVbaseAncestors().empty()) continue;
                uint64_t parentOff = parent->getSubObjectByteOffset(sup.get());
                llvm::Value* supThis = (parentOff == 0)
                    ? receiver
                    : builder->CreateInBoundsGEP(fi8Ty, receiver,
                        llvm::ConstantInt::get(fi64Ty, parentOff),
                        "vbase_fixup_supbase");
                llvm::Type* parentLlvm = sup->getLlvmType();
                for (auto& anc : sup->getVbaseAncestors()) {
                    if (!anc) continue;
                    int slotIdx = sup->getVbaseSlotIndex(anc.get());
                    if (slotIdx < 0) continue;
                    uint64_t canonOff = parent->getSubObjectByteOffset(anc.get());
                    llvm::Value* canonPtr = (canonOff == 0)
                        ? receiver
                        : builder->CreateInBoundsGEP(fi8Ty, receiver,
                            llvm::ConstantInt::get(fi64Ty, canonOff),
                            "vbase_fixup_canon");
                    llvm::Value* slotPtr = builder->CreateStructGEP(
                        parentLlvm, supThis, (unsigned) slotIdx,
                        "vbase_fixup_slot");
                    builder->CreateStore(canonPtr, slotPtr);
                }
            }
        }

        // Field initializers run between the super/this call and the ctor body. Skipped
        // when the ctor delegates via `this(args)`: the delegate runs them, and doing
        // it here would double-init and clobber whatever the delegate set.
        if (constructor && parent && bodyFn->arg_size() > 0 && block) {
            bool delegatesViaThis = false;
            std::function<bool(AbstractSyntaxNodePtr)> findThisCtor =
                [&](AbstractSyntaxNodePtr node) -> bool {
                    if (!node) return false;
                    if (auto mce = std::dynamic_pointer_cast<
                            cajeta::MethodCallExpression>(node)) {
                        if (mce->getMethodCallName() == "this") return true;
                    }
                    if (auto es = std::dynamic_pointer_cast<
                            cajeta::ExpressionStatement>(node)) {
                        if (findThisCtor(es->getExpression())) return true;
                    }
                    for (auto& child : node->getChildren()) {
                        if (findThisCtor(child)) return true;
                    }
                    return false;
                };
            delegatesViaThis = findThisCtor(block);

            if (!delegatesViaThis) {
                llvm::Value* thisPtr = bodyFn->getArg(0);
                auto& ictx = *module->getLlvmContext();
                for (auto& prop : parent->getPropertyList()) {
                    if (!prop || prop->isStatic()) continue;
                    auto init = prop->getInitializer();
                    if (!init) continue;
                    int idx = parent->getFieldLlvmIndex(prop);
                    if (idx < 0) continue;
                    llvm::Value* initVal = init->generateCode(module);
                    if (!initVal) {
                        // The field HAS an initializer and it lowered to nothing;
                        // continuing here would silently leave the field zero.
                        throw locatedException(
                            init->getSourceLine(), init->getSourceColumn() + 1,
                            "initializer for field '" + prop->getName()
                                + "' did not resolve to a value",
                            "CAJETA_ERROR_UNRESOLVED_EXPRESSION");
                    }
                    llvm::Value* fp = builder->CreateStructGEP(
                        parent->getLlvmType(), thisPtr, (unsigned) idx,
                        std::string("user_ctor.init.") + prop->getName());
                    // Width / FP coercion mirrors the synthesized-ctor path.
                    CajetaTypePtr ft = prop->getType();
                    llvm::Type* slotTy = ft ? ft->getLlvmType() : nullptr;
                    if (slotTy && initVal->getType() != slotTy) {
                        llvm::Type* srcTy = initVal->getType();
                        if (slotTy->isIntegerTy() && srcTy->isIntegerTy()) {
                            initVal = builder->CreateIntCast(initVal, slotTy, /*isSigned=*/true);
                        } else if (slotTy->isFloatingPointTy() && srcTy->isFloatingPointTy()) {
                            initVal = builder->CreateFPCast(initVal, slotTy);
                        } else if (slotTy->isFloatingPointTy() && srcTy->isIntegerTy()) {
                            initVal = builder->CreateSIToFP(initVal, slotTy);
                        } else if (slotTy->isIntegerTy() && srcTy->isFloatingPointTy()) {
                            initVal = builder->CreateFPToSI(initVal, slotTy);
                        }
                    }
                    builder->CreateStore(initVal, fp);
                }
                (void) ictx;
            }
        }

        // Type-resolver pre-pass (populates Expression::resolvedType), with a SCRIPT
        // module's semantic errors remapped into host coordinates at this boundary.
        try {
        if (block) {
            resolveBody(module);
            computeArenaEligibility();
            // Capture the arena mark in the entry region, where it dominates every
            // return: a block ending in a terminator skips its own reset, so this is
            // the only reclaim point for a method-direct or early-returned arena local.
            if (usesArena()) {
                if (llvm::Function* markFn =
                        module->getRuntimeFunction("__cajeta_arena_mark")) {
                    methodArenaMark = module->getBuilder()->CreateCall(
                        markFn, {}, "method.arena.mark");
                }
            }
            // The entry's body block is the session ROOT: its direct statements are
            // the session bindings.
            if (module->isScriptUnit() && name == scriptEntryName()) {
                module->armScriptRootBlock();
            }
            block->generateCode(module);
        }
        } catch (cajeta::Exception& e) {
            remapScriptException(module, e);
            throw;
        }

        // Emit a terminator only if the body did not. A missing return in a non-void
        // method is undefined in cajeta, but a zero-value ret keeps the IR well-formed.
        if (!builder->GetInsertBlock()->hasTerminator()) {
            // @After (then @AfterReturning) fires BEFORE scope_exit and the drops, so
            // advice can still read state the function owns. With an @Around wrapper
            // both fire there instead.
            if (!hasAroundWrapper) {
                emitAfterAdvice(module);
                emitAfterReturningAdvice(module);
                // Normal-return counterpart of the pop in emitAfterThrowingCatchArm.
                if (wrapBodyForAfterThrowing) {
                    emitAfterThrowingTryPop(module);
                }
            }
            // Pop every frame this method pushed by walking down to the watermark;
            // null when the frame was elided for a spawn-free body.
            if (scopeWatermark) {
                if (llvm::Function* exitToFn = module->getRuntimeFunction(
                        "__cajeta_scope_exit_to")) {
                    llvm::Value* mark = builder->CreateLoad(ptrTy, scopeWatermark);
                    builder->CreateCall(exitToFn, {mark});
                }
            }
            // Pop this method's debug frame on the fall-through path (--debug-info).
            dbg::emitDbgFrameLeave(module, dbgFrameSlot);
            // Pop the line-info shadow frame here too, or a fall-through method leaks
            // it into the next throw's trace. Only when the prologue pushed one.
            if (lineFrameEmitted) dbg::emitLineLeave(module);
            prof::emitProfileExit(module, profFrame);
            // Fire scope-end drops first, exactly as an explicit `return` would.
            emitOwnerDrops(module);
            // Use the FUNCTION's return type, not the CajetaType's: they diverge for
            // class returns (body struct vs the `ptr` ABI), and a struct-typed poison
            // against a `ptr` signature is a JIT-time verifier rejection.
            llvm::Function* hostFn = builder->GetInsertBlock()->getParent();
            llvm::Type* retLlvmTy = hostFn ? hostFn->getReturnType() : nullptr;
            if (!retLlvmTy || retLlvmTy->isVoidTy()) {
                builder->CreateRetVoid();
            } else if (retLlvmTy->isFloatingPointTy()) {
                builder->CreateRet(llvm::ConstantFP::getZero(retLlvmTy));
            } else if (retLlvmTy->isPointerTy()) {
                builder->CreateRet(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(retLlvmTy)));
            } else if (retLlvmTy->isIntegerTy()) {
                builder->CreateRet(llvm::ConstantInt::get(retLlvmTy, 0));
            } else {
                builder->CreateRet(llvm::PoisonValue::get(retLlvmTy));
            }
        }

        // Write ownership facts back to the session table while the entry's scope is
        // alive; a thrown compile error skips this and leaves the session unchanged.
        writeBackSessionState(module);

        destroyScope();
        if (pushedClass) {
            module->getStructureStack().pop_back();
        }

        // Catch arm for the body-level try frame — reached only via longjmp from a
        // throw inside the body; it re-raises so the throw still propagates.
        if (wrapBodyForAfterThrowing && bodyTryFrame.catchBB) {
            llvm::IRBuilder<> catchBuilder(bodyTryFrame.catchBB);
            emitAfterThrowingCatchArm(module, catchBuilder);
        }

        // The user body went into llvmOriginalFunction because @Around matched: emit
        // the wrapper into llvmFunction now. v1 honours the FIRST @Around match.
        if (hasAroundWrapper) {
            emitAroundWrapper();
        }

        // Size guard for the `alwaysinline` set at prototype time: now that the body
        // exists, measure it and drop the attribute when a large operator body would
        // bloat every call site. Plain (no-@Around) path only.
        if (!hasAroundWrapper && llvmFunction
                && llvmFunction->hasFnAttribute(llvm::Attribute::AlwaysInline)) {
            unsigned instrs = 0;
            for (auto& bb : *llvmFunction) instrs += (unsigned) bb.size();
            // Chosen so a typical value-type operator still inlines while a genuinely
            // large body keeps a real call.
            static const unsigned kValueOpInlineMaxInstrs = 100;
            if (instrs > kValueOpInlineMaxInstrs) {
                llvmFunction->removeFnAttr(llvm::Attribute::AlwaysInline);
            }
        }
    }

    // Append an around advice's transfer word: the target's word (last in `args`)
    // shifts up one, and bit 0 says whether the advice OWNS its proceed closure.
    static void appendAdviceTransferWord(llvm::IRBuilder<>& b, const MethodPtr& advice,
                                         std::vector<llvm::Value*>& args,
                                         bool proceedOwned) {
        if (!advice || !advice->needsTransferWord()) return;
        llvm::Function* fn = advice->getLlvmFunction();
        if (!fn) return;
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(b.getContext());
        llvm::Value* proceedBit = llvm::ConstantInt::get(i64Ty, proceedOwned ? 1 : 0);
        size_t want = fn->arg_size();
        if (args.size() == want) {
            llvm::Value* tw = args.back();
            if (tw->getType() == i64Ty) {
                args.back() = b.CreateOr(
                    b.CreateShl(tw, llvm::ConstantInt::get(i64Ty, 1)), proceedBit,
                    "advice_title_word");
                return;
            }
        }
        if (args.size() + 1 == want) {
            args.push_back(proceedBit);
        }
    }

    void Method::emitAroundWrapper() {
        auto& llvmFunction = llvmFunctionRef();
        auto& llvmFunctionType = llvmFunctionTypeRef();
        auto& llvmOriginalFunction = llvmOriginalFunctionRef();
        // matchingAdvice is already @Order-sorted: aroundChain[0] is the outermost and
        // aroundChain[N-1] wraps the original.
        std::vector<MethodPtr> aroundChain;
        for (auto& m : matchingAdvice) {
            if (m.kind == AdviceKind::Around && m.adviceMethod
                    && m.adviceMethod->getLlvmFunction()) {
                aroundChain.push_back(m.adviceMethod);
            }
        }
        if (aroundChain.empty()) {
            // Advice missing or not codegen-ready — leave the wrapper unimplemented.
            return;
        }

        auto& ctx = *module->getLlvmContext();
        auto* lmod = module->getLlvmModule();
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        const llvm::DataLayout& dl = lmod->getDataLayout();
        const size_t N = aroundChain.size();

        // The advice's @Original parameter is a closure record { ptr fn, ptr captures,
        // ptr drop_fn }: closure[k] wraps adapter[k] and captures closure[k+1], so each
        // adapter proceeds to the next inner advice; adapter[N-1] calls the original.

        // Adapter signature: llvmFunctionType plus a leading captures pointer.
        std::vector<llvm::Type*> adapterParamTys;
        adapterParamTys.push_back(ptrTy);   // captures
        for (auto* pt : llvmFunctionType->params()) {
            adapterParamTys.push_back(pt);
        }
        llvm::FunctionType* adapterFnTy = llvm::FunctionType::get(
            llvmFunctionType->getReturnType(), adapterParamTys, false);

        std::string baseName = getLlvmSymbolName();

        std::vector<llvm::Function*> adapters(N, nullptr);
        for (size_t k = 0; k < N; ++k) {
            std::string adapterName = baseName
                + "__around_adapter_" + std::to_string(k);
            adapters[k] = llvm::Function::Create(
                adapterFnTy, llvm::Function::ExternalLinkage,
                adapterName, lmod);
            llvm::BasicBlock* adBB = llvm::BasicBlock::Create(
                ctx, "entry", adapters[k]);
            llvm::IRBuilder<> adBuilder(adBB);
            llvm::Value* capturesArg = nullptr;
            std::vector<llvm::Value*> fwd;
            int i = 0;
            for (auto& a : adapters[k]->args()) {
                if (i++ == 0) { capturesArg = &a; continue; }
                fwd.push_back(&a);
            }
            llvm::CallInst* inner;
            if (k == N - 1) {
                inner = adBuilder.CreateCall(
                    llvmFunctionType, llvmOriginalFunction, fwd);
            } else {
                std::vector<llvm::Value*> nextArgs;
                nextArgs.push_back(capturesArg);
                for (auto* v : fwd) nextArgs.push_back(v);
                // An INNER advice is only LENT its proceed.
                appendAdviceTransferWord(adBuilder, aroundChain[k + 1],
                                         nextArgs, /*proceedOwned=*/false);
                inner = adBuilder.CreateCall(
                    aroundChain[k + 1]->getLlvmFunctionType(),
                    aroundChain[k + 1]->getLlvmFunction(),
                    nextArgs);
            }
            if (llvmFunctionType->getReturnType()->isVoidTy()) {
                adBuilder.CreateRetVoid();
            } else {
                adBuilder.CreateRet(inner);
            }
        }

        // Each closure is heap-allocated via __cajeta_alloc, the shape a lambda call
        // site expects.
        llvm::BasicBlock* wrapperBB = llvm::BasicBlock::Create(
            ctx, "wrapper", llvmFunction);
        llvm::IRBuilder<> wrapBuilder(wrapperBB);

        llvm::StructType* closureTy = llvm::StructType::get(ctx,
            { (llvm::Type*) ptrTy, (llvm::Type*) ptrTy, (llvm::Type*) ptrTy });
        llvm::Function* allocFn = module->getRuntimeFunction("__cajeta_alloc");
        if (!allocFn) {
            // Runtime not linked — bail; the verifier will flag the missing terminator.
            return;
        }

        // Allocate N closures innermost → outermost: closures[N-1] has captures null,
        // and closures[k] points its captures slot at closures[k+1].
        std::vector<llvm::Value*> closures(N, nullptr);
        for (size_t k = N; k-- > 0;) {
            llvm::Value* closure = wrapBuilder.CreateCall(allocFn, {
                llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(ctx),
                    dl.getTypeAllocSize(closureTy)),
            }, "around_closure_" + std::to_string(k));
            llvm::Value* fnSlot = wrapBuilder.CreateStructGEP(
                closureTy, closure, 0, "closure.fn");
            wrapBuilder.CreateStore(adapters[k], fnSlot);
            llvm::Value* capSlot = wrapBuilder.CreateStructGEP(
                closureTy, closure, 1, "closure.captures");
            llvm::Value* capValue = (k + 1 < N)
                ? closures[k + 1]
                : (llvm::Value*) llvm::ConstantPointerNull::get(ptrTy);
            wrapBuilder.CreateStore(capValue, capSlot);
            llvm::Value* dropSlot = wrapBuilder.CreateStructGEP(
                closureTy, closure, 2, "closure.drop_fn");
            // Only the OUTERMOST record drops, which frees the chain.
            llvm::Value* dropFnV = llvm::ConstantPointerNull::get(ptrTy);
            if (k == 0) {
                if (llvm::Function* chainFree = module->getRuntimeFunction(
                        "__cajeta_closure_chain_free")) {
                    dropFnV = chainFree;
                }
            }
            wrapBuilder.CreateStore(dropFnV, dropSlot);
            closures[k] = closure;
        }

        std::vector<llvm::Value*> callArgs;
        callArgs.push_back(closures[0]);
        for (auto& arg : llvmFunction->args()) {
            callArgs.push_back(&arg);
        }
        // The outer advice OWNS its proceed (bit 0).
        appendAdviceTransferWord(wrapBuilder, aroundChain[0], callArgs,
                                 /*proceedOwned=*/true);

        // @Before / @After / @AfterReturning emit at the WRAPPER level; they call
        // through module->getBuilder(), so swap it to the wrapper-local builder.
        auto* prevBuilder = builder;
        llvm::IRBuilder<>* wb = &wrapBuilder;
        builder = wb;
        module->setBuilder(wb);
        emitBeforeAdvice(module);

        // With @AfterThrowing matched, wrap the OUTERMOST advice call in a try frame:
        // a throw anywhere under the chain lands in the wrapper's catchBB.
        TryFrameInfo wrapTryFrame{nullptr, nullptr, nullptr};
        if (hasAfterThrowingAdvice()) {
            wrapTryFrame = emitAfterThrowingTryEntry(
                module, *wb, llvmFunction);
        }

        llvm::CallInst* result = wb->CreateCall(
            aroundChain[0]->getLlvmFunctionType(),
            aroundChain[0]->getLlvmFunction(),
            callArgs);

        if (wrapTryFrame.catchBB) {
            llvm::Function* pop = module->getRuntimeFunction("__cajeta_exc_pop");
            if (pop) wb->CreateCall(pop, {});
        }

        emitAfterAdvice(module);
        emitAfterReturningAdvice(module);

        builder = prevBuilder;
        module->setBuilder(prevBuilder);

        llvm::Type* retTy = llvmFunctionType->getReturnType();
        if (retTy->isVoidTy()) {
            wb->CreateRetVoid();
        } else {
            wb->CreateRet(result);
        }

        if (wrapTryFrame.catchBB) {
            llvm::IRBuilder<> catchBuilder(wrapTryFrame.catchBB);
            emitAfterThrowingCatchArm(module, catchBuilder);
        }
    }

    // Push this method's root scope, with its parent link deliberately cleared:
    // falling through that parent lets the resolveTypes pre-pass bind an identifier to
    // an unrelated method's same-named local and pin a wrong type on a shared node.
    void Method::createScope() {
        module->getScopeStack().add(make_shared<Scope>(toCanonical(), module));
        module->getScopeStack().peek()->setParent(nullptr);
    }

    // Pop this method's scope, first enforcing the launch-borrow gate (XPU-K02): a
    // device resource still borrowed by an unsynced launch must not be dropped here.
    void Method::destroyScope() {
        // Only locals with a live drop entry trip it; a sync clears the borrow set.
        if (ScopePtr sc = module->getScopeStack().peek()) {
            for (const string& name : sc->pendingLaunchBorrows()) {
                if (!sc->containsField(name) || sc->isBorrow(name)) continue;
                FieldPtr f = sc->getField(name);
                if (!f || !f->getDropEntry()) continue;
                // Formals are runtime owners and carry flag-armed entries, but the K02
                // rule keeps parameters out of this static gate.
                if (dynamic_pointer_cast<ParameterField>(f)) continue;
                CajetaTypePtr t = f->getType();
                if (!t) continue;
                // Every borrowed device resource — Buffer, Texture and acceleration
                // structure alike — must outlive the launch.
                const string c = t->toCanonical();
                bool isDeviceResource =
                    c.rfind("cajeta.xpu.KernelBuffer", 0) == 0 ||
                    c.rfind("cajeta.gfx.Texture2D", 0) == 0 ||
                    c.rfind("cajeta.gfx.Texture3D", 0) == 0 ||
                    c.rfind("cajeta.gfx.Texture1D", 0) == 0 ||
                    c.rfind("cajeta.gfx.Texture2DArray", 0) == 0 ||
                    c.rfind("cajeta.gfx.TextureCube", 0) == 0 ||
                    c == "cajeta.xpu.AccelerationStructure";
                if (!isDeviceResource) continue;
                throw Exception(
                    "device resource '" + name + "' leaves scope while a launch "
                    "still references it — sync the stream (Stream.sync()) before "
                    "it is dropped", "XPU-K02");
            }
        }
        module->getScopeStack().pop();
    }

    FieldPtr Method::getVariable(string name) {
        ScopePtr scope = module->getScopeStack().peek();
        return scope->getField(name);
    }


    // Type-arg suffix `<canonical,...>` for a method-template INSTANTIATION, so two
    // instantiations whose T-vars never appear in value parameters still get distinct
    // map keys and LLVM symbols. Empty for templates and ordinary methods.
    static string buildMethodTypeArgSuffix(
            const vector<CajetaTypePtr>& methodTypeArguments) {
        if (methodTypeArguments.empty()) {
            return "";
        }
        string s = "<";
        for (size_t i = 0; i < methodTypeArguments.size(); ++i) {
            if (i > 0) s += ",";
            s += methodTypeArguments[i]->getQName()->toCanonical();
        }
        s += ">";
        return s;
    }

    const string Method::getMapKey(bool labeled) const {
        // const_cast: toCanonical and the parameterList walk are read-only.
        string base = const_cast<Method*>(this)->toCanonical(labeled);
        if (methodTypeParameters.empty()) {
            return base + specializationTag;
        }
        if (!methodTypeArguments.empty()) {
            // Concrete instantiation: the type-args (plus any closure-specialization
            // tag) are what keep sibling instantiations' keys distinct.
            return base + buildMethodTypeArgSuffix(methodTypeArguments)
                 + specializationTag;
        }
        // A template DECLARATION suffixes the T-var NAMES, so its key cannot collide
        // with a same-value-param non-templated overload. Names rather than types keep
        // the suffix stable across re-parses; nothing compares it to user type args.
        string s = base + "<";
        for (size_t i = 0; i < methodTypeParameters.size(); ++i) {
            if (i > 0) s += ",";
            s += methodTypeParameters[i].name;
        }
        s += ">";
        return s;
    }

    const string Method::getLlvmSymbolName() const {
        return getMapKey(true);
    }

    // Drop a (closure-specialized-away) parameter from both the ordered list and the
    // by-name map, so the specialized instance's signature and canonical omit it.
    void Method::dropParameter(const string& paramName) {
        for (auto it = parameterList.begin(); it != parameterList.end(); ++it) {
            if (*it && (*it)->getName() == paramName) {
                parameterList.erase(it);
                break;
            }
        }
        parameters.erase(paramName);
    }

    // Canonical signature `Parent::name(type,...)`. `labeled` sorts the parameters by
    // name and prefixes each with `name:` — the labeled-overload key.
    string Method::buildCanonical(CajetaClassPtr parent, const string& name, vector<FormalParameterPtr> parameters, bool labeled) {
        string canonical;
        // symbolBase(): a redefined session class carries a generation suffix.
        canonical.append(parent->symbolBase());
        canonical.append("::");
        canonical.append(name);
        canonical.append("(");

        if (labeled) {
            sort(parameters.begin(), parameters.end(), [](FormalParameterPtr a, FormalParameterPtr b) {
                return a->getName() > b->getName();
            });
        }

        if (!parameters.empty()) {
            bool first = true;
            for (auto& parameter: parameters) {
                if (first) {
                    first = false;
                } else {
                    canonical.append(",");
                }
                if (labeled) {
                    canonical.append(parameter->getName()).append(":");
                }
                canonical.append(parameter->getType()->toCanonical());
            }
        }

        canonical.append(")");
        return canonical;
    }

    string Method::buildTemplateOriginCanonical(
            CajetaClassPtr instClass,
            const string& name,
            vector<FormalParameterPtr> parameters,
            bool labeled) {
        auto origin = instClass ? instClass->getTemplateOrigin() : nullptr;
        if (!origin) {
            return buildCanonical(instClass, name, parameters, labeled);
        }
        const auto& typeParams = instClass->getTypeParameters();
        const auto& typeArgs = instClass->getTypeArguments();
        // Identity match is the right comparison: instantiation stored the type
        // ARGUMENT's pointer as the field type, so a parameter whose type is in
        // typeArgs came from a template slot. Equal canonicals also map back.
        auto unsubstitute = [&](CajetaTypePtr t) -> string {
            if (!t) return "";
            if (typeParams.size() == typeArgs.size()) {
                for (size_t i = 0; i < typeArgs.size(); ++i) {
                    if (typeArgs[i].get() == t.get()) {
                        return typeParams[i].name;
                    }
                }
            }
            return t->toCanonical();
        };

        string canonical;
        canonical.append(origin->toCanonical());
        canonical.append("::");
        canonical.append(name);
        canonical.append("(");

        if (labeled) {
            sort(parameters.begin(), parameters.end(),
                [](FormalParameterPtr a, FormalParameterPtr b) {
                    return a->getName() > b->getName();
                });
        }

        if (!parameters.empty()) {
            bool first = true;
            for (auto& parameter : parameters) {
                if (first) first = false; else canonical.append(",");
                if (labeled) {
                    canonical.append(parameter->getName()).append(":");
                }
                canonical.append(unsubstitute(parameter->getType()));
            }
        }
        canonical.append(")");
        return canonical;
    }

    // Canonical signature built from resolved call arguments (the ParameterEntry form).
    string Method::buildCanonical(CajetaClassPtr parent, const string& name, vector<ParameterEntry> parameters, bool labeled) {
        string canonical;
        // symbolBase(): a redefined session class carries a generation suffix.
        canonical.append(parent->symbolBase());
        canonical.append("::");
        canonical.append(name);
        canonical.append("(");

        if (labeled) {
            sort(parameters.begin(), parameters.end(), [](ParameterEntry a, ParameterEntry b) {
                return a.label > b.label;
            });
        }

        if (!parameters.empty()) {
            bool first = true;
            for (auto& parameter: parameters) {
                if (first) {
                    first = false;
                } else {
                    canonical.append(",");
                }
                if (labeled) {
                    canonical.append(parameter.label).append(":");
                }
                canonical.append(parameter.type->toCanonical());
            }
        }

        canonical.append(")");
        return canonical;
    }

    // Generic-form signature: the erased (`toGeneric`) counterpart of buildCanonical.
    string Method::buildGeneric(CajetaClassPtr parent, const string& name, vector<FormalParameterPtr> parameters, bool labeled) {
        string canonical;
        // symbolBase(): a redefined session class carries a generation suffix.
        canonical.append(parent->symbolBase());
        canonical.append("::");
        canonical.append(name);
        canonical.append("(");

        if (labeled) {
            sort(parameters.begin(), parameters.end(), [](FormalParameterPtr a, FormalParameterPtr b) {
                return a->getName() > b->getName();
            });
        }

        if (!parameters.empty()) {
            bool first = true;
            for (auto& parameter: parameters) {
                if (first) {
                    first = false;
                } else {
                    canonical.append(",");
                }
                if (labeled) {
                    canonical.append(parameter->getName()).append(":");
                }
                canonical.append(parameter->getType()->toGeneric());
            }
        }

        canonical.append(")");
        return canonical;
    }

    // Generic-form signature built from resolved call arguments.
    string Method::buildGeneric(CajetaClassPtr parent, const string& name, vector<ParameterEntry> parameters, bool labeled) {
        string canonical;
        // symbolBase(): a redefined session class carries a generation suffix.
        canonical.append(parent->symbolBase());
        canonical.append("::");
        canonical.append(name);
        canonical.append("(");

        if (labeled) {
            sort(parameters.begin(), parameters.end(), [](ParameterEntry a, ParameterEntry b) {
                return a.label > b.label;
            });
        }

        if (!parameters.empty()) {
            bool first = true;
            for (const ParameterEntry& parameter: parameters) {
                if (first) {
                    first = false;
                } else {
                    canonical.append(",");
                }
                if (labeled) {
                    canonical.append(parameter.label).append(":");
                }

                canonical.append(parameter.type->toGeneric());
            }
        }

        canonical.append(")");
        return canonical;
    }

    // Factory: build a method and wire each formal's parent and by-name map entry.
    MethodPtr Method::create(CajetaModulePtr module,
        string& name,
        CajetaTypePtr returnType,
        vector<FormalParameterPtr> parameters,
        BlockPtr block,
        CajetaClassPtr parent) {
        MethodPtr result = make_shared<Method>(module, name, returnType, parameters, block, parent);
        for (auto& parameter: result->parameterList) {
            parameter->setParent(result);
            result->parameters[parameter->getName()] = parameter;
        }
        return result;
    }

    MethodPtr Method::create(CajetaModulePtr module,
        string name,
        CajetaTypePtr returnType,
        CajetaClassPtr parent) {
            return make_shared<Method>(module, name, returnType, parent);
    }

}