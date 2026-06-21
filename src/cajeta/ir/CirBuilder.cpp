//
// CirBuilder — AST -> CIR lowering for the closed slice. See CirBuilder.h.
//

#include "CirBuilder.h"

#include <string>
#include <unordered_map>

#include "cajeta/method/Method.h"
#include "cajeta/type/FormalParameter.h"
#include "cajeta/type/CajetaType.h"
#include "cajeta/type/CajetaFunctionType.h"

#include "cajeta/asn/AbstractSyntaxNode.h"
#include "cajeta/asn/Block.h"
#include "cajeta/asn/Statement.h"
#include "cajeta/asn/LocalVariableDeclaration.h"
#include "cajeta/asn/VariableDeclarator.h"
#include "cajeta/asn/expression/Expression.h"
#include "cajeta/asn/expression/Identifier.h"
#include "cajeta/asn/expression/BinaryOpExpression.h"
#include "cajeta/asn/expression/MethodCallExpression.h"

namespace cajeta {
namespace ir {

namespace {

    template <typename T>
    std::shared_ptr<T> as(const AbstractSyntaxNodePtr& n) {
        return std::dynamic_pointer_cast<T>(n);
    }

    CajetaTypePtr resolvedTypeOf(const AbstractSyntaxNodePtr& n) {
        if (auto e = as<Expression>(n)) return e->getResolvedType();
        return nullptr;
    }

    bool isVoidType(const CajetaTypePtr& t) {
        if (!t) return true;
        const std::string c = t->toCanonical();
        if (c == "void") return true;
        return c.size() >= 5 && c.compare(c.size() - 5, 5, ".void") == 0;
    }

    CirType cirTypeOf(const CajetaTypePtr& t) {
        CirType ct;
        ct.spelling = t ? t->toCanonical() : "?";
        ct.resolved = t;
        return ct;
    }

    CirOwnership ownershipFor(const CajetaTypePtr& t, bool transferred) {
        if (transferred) return CirOwnership::Owned;
        if (!t) return CirOwnership::Value;
        if (std::dynamic_pointer_cast<CajetaFunctionType>(t)) return CirOwnership::Value;
        if (t->getTypeFlags() & (PRIMITIVE_FLAG | VALUE_TYPE_FLAG)) return CirOwnership::Value;
        return CirOwnership::Borrowed;   // class / array / interface reference
    }

    // Lowering state for one function.
    struct Lowerer {
        CirFunctionPtr fn = std::make_shared<CirFunction>();
        CirBlockPtr block = std::make_shared<CirBlock>();
        std::unordered_map<std::string, CirValuePtr> scope;
        int temp = 0;
        int lambdaSeq = 0;

        CirValuePtr fresh(const CirType& ty, CirOwnership own = CirOwnership::Value) {
            auto v = std::make_shared<CirValue>();
            v->name = "t" + std::to_string(temp++);
            v->type = ty;
            v->ownership = own;
            return v;
        }
        void emit(const CirInstPtr& i) { block->insts.push_back(i); }

        AbstractSyntaxNodePtr child(const AbstractSyntaxNodePtr& n, size_t i) {
            auto& cs = n->getChildren();
            return i < cs.size() ? cs[i] : nullptr;
        }

        // ---- expressions ---------------------------------------------------

        CirValuePtr lowerExpr(const AbstractSyntaxNodePtr& node) {
            if (!node) return nullptr;

            if (auto id = as<IdentifierExpression>(node)) {
                auto it = scope.find(id->getTextValue());
                if (it != scope.end()) return it->second;
                return fresh(cirTypeOf(id->getResolvedType()));   // unknown name -> opaque
            }
            if (auto lam = as<LambdaExpression>(node)) return lowerLambda(lam);
            if (auto call = as<MethodCallExpression>(node)) return lowerCall(call);
            if (auto bin = as<BinaryOpExpression>(node)) return lowerBinary(bin);

            if (as<ArrayIndexExpression>(node)) {
                auto arr = lowerExpr(child(node, 0));
                auto idx = lowerExpr(child(node, 1));
                auto inst = cirInst(CirOp::ElemAddr);
                if (arr) inst->operands.push_back(arr);
                if (idx) inst->operands.push_back(idx);
                auto r = fresh(cirTypeOf(resolvedTypeOf(node)));
                inst->result = r;
                emit(inst);
                return r;
            }
            if (as<MoveExpression>(node)) {
                auto v = lowerExpr(child(node, 0));
                auto inst = cirInst(CirOp::Move);
                if (v) inst->operands.push_back(v);
                auto r = fresh(cirTypeOf(resolvedTypeOf(node)), CirOwnership::Owned);
                inst->result = r;
                emit(inst);
                return r;
            }
            if (as<CastExpression>(node)) {
                return lowerExpr(child(node, 0));   // best-effort pass-through
            }
            if (as<BooleanSwitchExpression>(node)) {  // ternary
                lowerExpr(child(node, 0));
                lowerExpr(child(node, 1));
                lowerExpr(child(node, 2));
                return fresh(cirTypeOf(resolvedTypeOf(node)));
            }
            if (as<PrimaryExpression>(node)) {
                CirValuePtr last;
                bool any = false;
                for (auto& c : node->getChildren())
                    if (as<Expression>(c)) { last = lowerExpr(c); any = true; }
                if (any) return last;
                // a leaf literal — emit a constant placeholder (value not modeled in Phase A)
                auto inst = cirInst(CirOp::ConstInt);
                auto r = fresh(cirTypeOf(resolvedTypeOf(node)));
                inst->result = r;
                emit(inst);
                return r;
            }

            // generic fallback: recurse Expression children to discover nested
            // closures/calls, then return an opaque value.
            CirValuePtr last;
            for (auto& c : node->getChildren())
                if (as<Expression>(c)) last = lowerExpr(c);
            if (last) return last;
            return fresh(cirTypeOf(resolvedTypeOf(node)));
        }

        CirValuePtr lowerLambda(const std::shared_ptr<LambdaExpression>& lam) {
            auto inst = cirInst(CirOp::MakeClosure);
            // synthesizedName isn't set until codegen (after this pass), so name
            // the target deterministically from the enclosing function.
            inst->symbol = fn->name + "$lambda" + std::to_string(lambdaSeq++);
            // §2.4.1: a non-capturing lambda has a statically-known unique target.
            // Captures aren't enumerated in Phase A (only knownness matters).
            inst->targetKnown = !lam->getHasBorrowCaptures();
            auto r = fresh(cirTypeOf(lam->getResolvedType()), CirOwnership::Value);
            inst->result = r;
            emit(inst);
            return r;
        }

        CirValuePtr lowerCall(const std::shared_ptr<MethodCallExpression>& call) {
            const std::string& name = call->getMethodCallName();

            // Closure call: the callee name resolves to a function-typed value in
            // scope (a parameter or local) -> apply.closure (§2.3.5).
            auto it = scope.find(name);
            std::shared_ptr<CajetaFunctionType> fnTy;
            if (it != scope.end())
                fnTy = std::dynamic_pointer_cast<CajetaFunctionType>(it->second->type.resolved);

            if (fnTy) {
                auto inst = cirInst(CirOp::ApplyClosure);
                inst->operands.push_back(it->second);       // the closure value
                for (auto& p : call->getParameters())
                    if (auto a = lowerExpr(p.expression)) inst->operands.push_back(a);
                auto r = fresh(cirTypeOf(fnTy->getReturnType()));
                inst->result = r;
                emit(inst);
                return r;
            }

            // Direct named call.
            auto inst = cirInst(CirOp::Call);
            inst->symbol = name;
            for (auto& p : call->getParameters())
                if (auto a = lowerExpr(p.expression)) inst->operands.push_back(a);
            auto retTy = call->getResolvedType();
            CirValuePtr r;
            if (!isVoidType(retTy)) {
                r = fresh(cirTypeOf(retTy));
                inst->result = r;
            }
            emit(inst);
            return r;
        }

        CirValuePtr lowerBinary(const std::shared_ptr<BinaryOpExpression>& bin) {
            auto lhs = lowerExpr(child(bin, 0));
            auto rhs = lowerExpr(child(bin, 1));
            BinaryOp op = bin->getBinaryOp();

            if (isAssignOp(op)) {
                // model as a store into the lvalue; the expression value is the rhs
                auto inst = cirInst(CirOp::Store);
                if (lhs) inst->operands.push_back(lhs);
                if (rhs) inst->operands.push_back(rhs);
                emit(inst);
                return rhs ? rhs : lhs;
            }

            std::string pred;
            CirOp cop = mapBinaryOp(op, pred);
            auto inst = cirInst(cop);
            if (!pred.empty()) inst->symbol = pred;
            if (lhs) inst->operands.push_back(lhs);
            if (rhs) inst->operands.push_back(rhs);
            auto r = fresh(cirTypeOf(bin->getResolvedType()));
            inst->result = r;
            emit(inst);
            return r;
        }

        static bool isAssignOp(BinaryOp op) {
            switch (op) {
                case BINARY_OP_ASSIGN:
                case BINARY_OP_ADD_EQUALS:
                case BINARY_OP_SUB_EQUALS:
                case BINARY_OP_MUL_EQUALS:
                case BINARY_OP_DIV_EQUALS:
                case BINARY_OP_BITAND_EQUALS:
                case BINARY_OP_BITOR_EQUALS:
                case BINARY_OP_BITXOR_EQUALS:
                case BINARY_OP_SHIFTRIGHT_EQUALS:
                case BINARY_OP_USHIFTRIGHT_EQUALS:
                case BINARY_OP_SHIFTLEFT_EQUALS:
                case BINARY_OP_MOD_EQUALS:
                    return true;
                default:
                    return false;
            }
        }

        // Maps an arithmetic/compare BinaryOp to a CirOp; sets `pred` for compares.
        static CirOp mapBinaryOp(BinaryOp op, std::string& pred) {
            switch (op) {
                case BINARY_OP_ADD:          return CirOp::Add;
                case BINARY_OP_SUB:          return CirOp::Sub;
                case BINARY_OP_MUL:          return CirOp::Mul;
                case BINARY_OP_DIV:          return CirOp::Div;
                case BINARY_OP_MOD:          return CirOp::Rem;
                case BINARY_OP_BITAND:
                case BINARY_OP_LOGAND:       return CirOp::And;
                case BINARY_OP_BITOR:
                case BINARY_OP_LOGOR:        return CirOp::Or;
                case BINARY_OP_BITXOR:       return CirOp::Xor;
                case BINARY_OP_SHIFTLEFT:    return CirOp::Shl;
                case BINARY_OP_SHIFTRIGHT:
                case BINARY_OP_USHIFTRIGHT:  return CirOp::Shr;
                case BINARY_OP_LT: pred = "slt"; return CirOp::ICmp;
                case BINARY_OP_LE: pred = "sle"; return CirOp::ICmp;
                case BINARY_OP_GT: pred = "sgt"; return CirOp::ICmp;
                case BINARY_OP_GE: pred = "sge"; return CirOp::ICmp;
                case BINARY_OP_EQ: pred = "eq";  return CirOp::ICmp;
                case BINARY_OP_NE: pred = "ne";  return CirOp::ICmp;
                default:                     return CirOp::Add;   // unreachable for the slice
            }
        }

        // ---- statements ----------------------------------------------------

        void lowerStmt(const AbstractSyntaxNodePtr& node) {
            if (!node) return;

            if (auto b = as<Block>(node)) {
                for (auto& c : b->getChildren()) lowerStmt(c);
                return;
            }
            // A braced `{ ... }` in statement position parses to a LabelStatement
            // (and `scope { ... }` to a ScopeStatement); the Block is held outside
            // `children`, so descend via getBlock().
            if (auto ls = as<LabelStatement>(node)) { lowerStmt(ls->getBlock()); return; }
            if (auto sc = as<ScopeStatement>(node)) { lowerStmt(sc->getBlock()); return; }
            if (auto lvd = as<LocalVariableDeclaration>(node)) { lowerLocalDecl(lvd); return; }
            if (auto es = as<ExpressionStatement>(node)) { lowerExpr(es->getExpression()); return; }
            if (auto rs = as<ReturnStatement>(node)) {
                if (rs->getExpression()) lowerExpr(rs->getExpression());
                return;
            }
            if (auto is = as<IfStatement>(node)) {
                lowerExpr(is->getCondition());
                lowerStmt(is->getThenBranch());
                lowerStmt(is->getElseBranch());
                return;
            }
            if (auto ws = as<WhileStatement>(node)) {
                lowerExpr(ws->getCondition());
                lowerStmt(ws->getBody());
                return;
            }
            if (auto fs = as<ForStatement>(node)) {
                lowerStmt(fs->getInit());
                if (fs->getCondition()) lowerExpr(fs->getCondition());
                for (auto& u : fs->getUpdate()) lowerExpr(u);
                lowerStmt(fs->getBody());
                return;
            }
            if (auto efs = as<EnhancedForStatement>(node)) {
                lowerExpr(efs->getIterableExpr());
                lowerStmt(efs->getBody());
                return;
            }

            // fallback: recurse children (covers nested blocks and statements
            // whose sub-parts live in `children`).
            for (auto& c : node->getChildren()) {
                if (as<Expression>(c)) lowerExpr(c);
                else lowerStmt(c);
            }
        }

        void lowerLocalDecl(const std::shared_ptr<LocalVariableDeclaration>& lvd) {
            auto declTy = lvd->getType();
            for (auto& d : lvd->getVariableDeclarators()) {
                CirValuePtr val;
                if (auto initzr = d->getInitializer()) {
                    AbstractSyntaxNodePtr expr =
                        initzr->getChildren().empty() ? nullptr : initzr->getChildren()[0];
                    val = lowerExpr(expr);
                }
                // bind a stable memory-cell value under the identifier (single
                // definition via alloc.stack; reads return this value, writes store).
                auto local = std::make_shared<CirValue>();
                local->name = d->getIdentifier();
                local->type = cirTypeOf(declTy);
                local->ownership = ownershipFor(declTy, false);
                scope[d->getIdentifier()] = local;

                auto al = cirInst(CirOp::AllocStack);
                al->result = local;
                emit(al);
                if (val) {
                    auto st = cirInst(CirOp::Store);
                    st->operands = {local, val};
                    emit(st);
                }
            }
        }
    };

} // namespace

CirFunctionPtr CirBuilder::buildFunction(const MethodPtr& method) {
    if (!method) return nullptr;

    Lowerer L;
    L.fn->name = method->getName();
    for (auto& tp : method->getMethodTypeParameters())
        L.fn->genericParams.push_back(tp.name);

    for (auto& p : method->getParameterList()) {
        auto v = std::make_shared<CirValue>();
        v->name = p->getName();
        v->type = cirTypeOf(p->getType());
        v->ownership = ownershipFor(p->getType(), p->isTransferred());
        L.fn->params.push_back(v);
        L.scope[p->getName()] = v;
    }

    auto rt = method->getReturnType();
    L.fn->returnType = isVoidType(rt) ? CirType::voidType() : cirTypeOf(rt);
    L.fn->returnOwnership =
        method->isReturnsOwnership() ? CirOwnership::Owned : CirOwnership::Value;

    L.block->label = "bb0";
    L.block->params = L.fn->params;     // entry-block params alias the function params
    L.fn->blocks.push_back(L.block);

    if (method->getBlock()) L.lowerStmt(method->getBlock());

    // Phase A is a single block; a `return` terminator keeps the CFG well-formed
    // (the lowering is structural for closure analysis, not a faithful CFG).
    L.block->terminator = cirInst(CirOp::Return);

    return L.fn;
}

} // namespace ir
} // namespace cajeta
