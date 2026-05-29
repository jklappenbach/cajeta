//
// NVPTX kernel lowering — see header.
//

#include "NvptxKernelLowering.h"

#include "../../method/Method.h"
#include "../../type/FormalParameter.h"
#include "../../type/CajetaType.h"
#include "../../type/CajetaClass.h"
#include "../../error/Exception.h"

#include "../../asn/AbstractSyntaxNode.h"
#include "../../asn/Block.h"
#include "../../asn/Statement.h"
#include "../../asn/LocalVariableDeclaration.h"
#include "../../asn/VariableDeclarator.h"
#include "../../asn/expression/Expression.h"
#include "../../asn/expression/Identifier.h"
#include "../../asn/expression/MethodCallExpression.h"
#include "../../asn/expression/BinaryOpExpression.h"
#include "../../asn/expression/LiteralExpression.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsNVPTX.h"
#include "llvm/IR/Module.h"

#include <map>
#include <string>

namespace cajeta {
namespace xpu {
namespace nvidia {

namespace {

[[noreturn]] void unsupported(const std::string& what) {
    throw cajeta::Exception(
        "NVPTX kernel lowering: unsupported construct — " + what,
        "XPU-N01");
}

// addrspace(1) == CUDA global memory (AddressSpace.h nvidiaNumberFor).
constexpr unsigned kGlobalAS = 1;

// Map a primitive CajetaType to a device LLVM scalar type, built fresh
// in the device context (NOT via CajetaType::getLlvmType(), whose cache
// is bound to the host context). Returns nullptr for non-primitives.
llvm::Type* deviceScalarType(const CajetaTypePtr& t, llvm::LLVMContext& ctx) {
    if (!t) return nullptr;
    unsigned long f = t->getTypeFlags();
    if (!(f & PRIMITIVE_FLAG)) return nullptr;
    if (f & FLOAT_FLAG) {
        if (f & BIT_64_FLAG) return llvm::Type::getDoubleTy(ctx);
        if (f & BIT_16_FLAG) return llvm::Type::getHalfTy(ctx);
        return llvm::Type::getFloatTy(ctx);            // BIT_32 default
    }
    if (f & INT_FLAG) {
        if (f & BIT_64_FLAG) return llvm::Type::getInt64Ty(ctx);
        if (f & BIT_32_FLAG) return llvm::Type::getInt32Ty(ctx);
        if (f & BIT_16_FLAG) return llvm::Type::getInt16Ty(ctx);
        if (f & BIT_8_FLAG)  return llvm::Type::getInt8Ty(ctx);
        return llvm::Type::getInt1Ty(ctx);             // boolean (no BIT flag)
    }
    return nullptr;
}

bool isBufferType(const CajetaTypePtr& t) {
    if (!t) return false;
    const std::string& c = t->toCanonical();
    static const std::string kPrefix = "cajeta.xpu.core.Buffer";
    return c.compare(0, kPrefix.size(), kPrefix) == 0;
}

bool typeIsSigned(const CajetaTypePtr& t) {
    return t && (t->getTypeFlags() & SIGNED_FLAG);
}

// One device kernel's worth of lowering state.
class DeviceLowerer {
public:
    DeviceLowerer(llvm::Module& m, llvm::Function* fn)
        : mod(m), ctx(m.getContext()), builder(ctx), fn(fn) {}

    void lowerBody(const MethodPtr& method) {
        builder.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", fn));
        lowerStatement(method->getBlock());
        // Kernels return void; close any open block.
        if (!builder.GetInsertBlock()->getTerminator()) {
            builder.CreateRetVoid();
        }
    }

    // Bind a parameter value (and, for buffers, its element type).
    void bindParam(const std::string& name, llvm::Value* v,
                   bool isSigned, llvm::Type* bufferElem) {
        values[name] = v;
        signedness[name] = isSigned;
        if (bufferElem) bufferElems[name] = bufferElem;
    }

private:
    llvm::Module& mod;
    llvm::LLVMContext& ctx;
    llvm::IRBuilder<> builder;
    llvm::Function* fn;
    std::map<std::string, llvm::Value*> values;
    std::map<std::string, bool> signedness;
    std::map<std::string, llvm::Type*> bufferElems;  // base name -> element type

    llvm::Value* readSreg(llvm::Intrinsic::ID id) {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(&mod, id);
        return builder.CreateCall(f, {}, "sreg");
    }

    // ---- statements -----------------------------------------------------

    void lowerStatement(const AbstractSyntaxNodePtr& node) {
        if (!node) return;
        if (auto blk = std::dynamic_pointer_cast<Block>(node)) {
            for (auto& s : blk->getChildren()) lowerStatement(s);
            return;
        }
        if (auto ls = std::dynamic_pointer_cast<LabelStatement>(node)) {
            lowerStatement(ls->getBlock());
            return;
        }
        if (auto lvd = std::dynamic_pointer_cast<LocalVariableDeclaration>(node)) {
            lowerLocalDecl(lvd);
            return;
        }
        if (auto ifs = std::dynamic_pointer_cast<IfStatement>(node)) {
            lowerIf(ifs);
            return;
        }
        if (auto es = std::dynamic_pointer_cast<ExpressionStatement>(node)) {
            lowerExprStatement(es->getExpression());
            return;
        }
        if (std::dynamic_pointer_cast<ReturnStatement>(node)) {
            // @Kernel returns void; a bare `return;` just closes the block.
            builder.CreateRetVoid();
            return;
        }
        unsupported("statement form in kernel body");
    }

    void lowerLocalDecl(const std::shared_ptr<LocalVariableDeclaration>& lvd) {
        CajetaTypePtr declType = lvd->getType();
        for (auto& vd : lvd->getVariableDeclarators()) {
            if (!vd) continue;
            auto init = vd->getInitializer();
            if (!init || init->getChildren().empty()) {
                unsupported("local declaration without initializer");
            }
            auto initExpr = std::dynamic_pointer_cast<Expression>(
                init->getChildren()[0]);
            llvm::Value* v = lowerExpr(initExpr);
            values[vd->getIdentifier()] = v;
            signedness[vd->getIdentifier()] = typeIsSigned(declType);
        }
    }

    void lowerIf(const std::shared_ptr<IfStatement>& ifs) {
        llvm::Value* cond = lowerExpr(ifs->getCondition());
        if (!cond->getType()->isIntegerTy(1)) {
            cond = builder.CreateICmpNE(
                cond, llvm::ConstantInt::get(cond->getType(), 0));
        }
        auto* thenBB = llvm::BasicBlock::Create(ctx, "if.then", fn);
        auto* endBB  = llvm::BasicBlock::Create(ctx, "if.end", fn);
        llvm::BasicBlock* elseBB =
            ifs->getElseBranch() ? llvm::BasicBlock::Create(ctx, "if.else", fn)
                                 : endBB;
        builder.CreateCondBr(cond, thenBB, elseBB);

        builder.SetInsertPoint(thenBB);
        lowerStatement(ifs->getThenBranch());
        if (!builder.GetInsertBlock()->getTerminator()) builder.CreateBr(endBB);

        if (ifs->getElseBranch()) {
            builder.SetInsertPoint(elseBB);
            lowerStatement(ifs->getElseBranch());
            if (!builder.GetInsertBlock()->getTerminator()) builder.CreateBr(endBB);
        }
        builder.SetInsertPoint(endBB);
    }

    void lowerExprStatement(const ExpressionPtr& expr) {
        // Assignment to a buffer element: `buf[i] = rhs`.
        if (auto bin = std::dynamic_pointer_cast<BinaryOpExpression>(expr)) {
            if (bin->getBinaryOp() == BINARY_OP_ASSIGN) {
                auto lhs = exprChild(bin, 0);
                auto rhs = exprChild(bin, 1);
                auto [addr, elemTy] = lowerLValueAddr(lhs);
                llvm::Value* val = coerceTo(lowerExpr(rhs), elemTy);
                builder.CreateStore(val, addr);
                return;
            }
        }
        // Any other top-level expression (e.g. a bare builtin call) — lower
        // for side effects, discard the value.
        lowerExpr(expr);
    }

    // ---- expressions ----------------------------------------------------

    llvm::Value* lowerExpr(const ExpressionPtr& expr) {
        if (!expr) unsupported("null expression");

        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(expr)) {
            auto it = values.find(id->getTextValue());
            if (it == values.end()) unsupported("unbound identifier '" +
                                                id->getTextValue() + "'");
            return it->second;
        }
        if (auto il = std::dynamic_pointer_cast<IntegerLiteralExpression>(expr)) {
            return llvm::ConstantInt::get(
                llvm::Type::getInt32Ty(ctx),
                std::stoll(stripSuffix(il->getRawValue())), /*signed=*/true);
        }
        if (auto fl = std::dynamic_pointer_cast<FloatLiteralExpression>(expr)) {
            return llvm::ConstantFP::get(
                llvm::Type::getFloatTy(ctx),
                std::stod(stripSuffix(fl->getRawValue())));
        }
        if (auto mc = std::dynamic_pointer_cast<MethodCallExpression>(expr)) {
            return lowerBuiltinCall(mc);
        }
        if (auto ai = std::dynamic_pointer_cast<ArrayIndexExpression>(expr)) {
            auto [addr, elemTy] = lowerLValueAddr(ai);
            return builder.CreateLoad(elemTy, addr, "elem");
        }
        if (auto bin = std::dynamic_pointer_cast<BinaryOpExpression>(expr)) {
            return lowerBinaryOp(bin);
        }
        unsupported("expression form in kernel body");
    }

    // Address (and element type) of an l-value. Only buffer/array indexing
    // is supported as an assignment target in the slice.
    std::pair<llvm::Value*, llvm::Type*> lowerLValueAddr(const ExpressionPtr& e) {
        auto ai = std::dynamic_pointer_cast<ArrayIndexExpression>(e);
        if (!ai) unsupported("l-value that isn't a buffer index");
        auto baseId = std::dynamic_pointer_cast<IdentifierExpression>(
            exprChild(ai, 0));
        if (!baseId) unsupported("buffer index on a non-identifier base");
        auto bv = values.find(baseId->getTextValue());
        auto be = bufferElems.find(baseId->getTextValue());
        if (bv == values.end() || be == bufferElems.end()) {
            unsupported("index on non-buffer '" + baseId->getTextValue() + "'");
        }
        llvm::Value* idx = lowerExpr(exprChild(ai, 1));
        // GEP wants an i64 index.
        if (idx->getType() != llvm::Type::getInt64Ty(ctx)) {
            idx = builder.CreateZExt(idx, llvm::Type::getInt64Ty(ctx));
        }
        llvm::Value* addr = builder.CreateGEP(be->second, bv->second, {idx}, "idx");
        return {addr, be->second};
    }

    llvm::Value* lowerBuiltinCall(const std::shared_ptr<MethodCallExpression>& mc) {
        std::string recv;
        if (!mc->getChildren().empty()) {
            if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(
                    mc->getChildren()[0])) {
                recv = id->getTextValue();
            }
        }
        const std::string& name = mc->getMethodCallName();
        using I = llvm::Intrinsic::ID;

        auto tid = [&](I id) { return readSreg(id); };
        auto global = [&](I ctaid, I ntid, I t) {
            // ctaid * ntid + tid
            llvm::Value* m = builder.CreateMul(readSreg(ctaid), readSreg(ntid));
            return builder.CreateAdd(m, readSreg(t));
        };

        if (recv == "Thread") {
            if (name == "x") return tid(llvm::Intrinsic::nvvm_read_ptx_sreg_tid_x);
            if (name == "y") return tid(llvm::Intrinsic::nvvm_read_ptx_sreg_tid_y);
            if (name == "z") return tid(llvm::Intrinsic::nvvm_read_ptx_sreg_tid_z);
            if (name == "globalIdX")
                return global(llvm::Intrinsic::nvvm_read_ptx_sreg_ctaid_x,
                              llvm::Intrinsic::nvvm_read_ptx_sreg_ntid_x,
                              llvm::Intrinsic::nvvm_read_ptx_sreg_tid_x);
            if (name == "globalIdY")
                return global(llvm::Intrinsic::nvvm_read_ptx_sreg_ctaid_y,
                              llvm::Intrinsic::nvvm_read_ptx_sreg_ntid_y,
                              llvm::Intrinsic::nvvm_read_ptx_sreg_tid_y);
            if (name == "globalIdZ")
                return global(llvm::Intrinsic::nvvm_read_ptx_sreg_ctaid_z,
                              llvm::Intrinsic::nvvm_read_ptx_sreg_ntid_z,
                              llvm::Intrinsic::nvvm_read_ptx_sreg_tid_z);
        } else if (recv == "Workgroup") {
            if (name == "x") return tid(llvm::Intrinsic::nvvm_read_ptx_sreg_ctaid_x);
            if (name == "y") return tid(llvm::Intrinsic::nvvm_read_ptx_sreg_ctaid_y);
            if (name == "z") return tid(llvm::Intrinsic::nvvm_read_ptx_sreg_ctaid_z);
            if (name == "dimX") return tid(llvm::Intrinsic::nvvm_read_ptx_sreg_ntid_x);
            if (name == "dimY") return tid(llvm::Intrinsic::nvvm_read_ptx_sreg_ntid_y);
            if (name == "dimZ") return tid(llvm::Intrinsic::nvvm_read_ptx_sreg_ntid_z);
        } else if (recv == "Barrier") {
            if (name == "workgroup") {
                // bar.sync 0 — synchronize all threads in the CTA.
                llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
                    &mod, llvm::Intrinsic::nvvm_barrier_cta_sync_aligned_all);
                builder.CreateCall(f, {llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(ctx), 0)});
                return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
            }
        }
        unsupported("device builtin '" + recv + "." + name + "()'");
    }

    llvm::Value* lowerBinaryOp(const std::shared_ptr<BinaryOpExpression>& bin) {
        llvm::Value* l = lowerExpr(exprChild(bin, 0));
        llvm::Value* r = lowerExpr(exprChild(bin, 1));
        bool fp = l->getType()->isFloatingPointTy() ||
                  r->getType()->isFloatingPointTy();
        bool sign = exprSigned(exprChild(bin, 0)) || exprSigned(exprChild(bin, 1));
        switch (bin->getBinaryOp()) {
            case BINARY_OP_ADD: return fp ? builder.CreateFAdd(l, r)
                                          : builder.CreateAdd(l, r);
            case BINARY_OP_SUB: return fp ? builder.CreateFSub(l, r)
                                          : builder.CreateSub(l, r);
            case BINARY_OP_MUL: return fp ? builder.CreateFMul(l, r)
                                          : builder.CreateMul(l, r);
            case BINARY_OP_DIV:
                return fp ? builder.CreateFDiv(l, r)
                          : (sign ? builder.CreateSDiv(l, r)
                                  : builder.CreateUDiv(l, r));
            case BINARY_OP_LT:
                return fp ? builder.CreateFCmpOLT(l, r)
                          : (sign ? builder.CreateICmpSLT(l, r)
                                  : builder.CreateICmpULT(l, r));
            case BINARY_OP_LE:
                return fp ? builder.CreateFCmpOLE(l, r)
                          : (sign ? builder.CreateICmpSLE(l, r)
                                  : builder.CreateICmpULE(l, r));
            case BINARY_OP_GT:
                return fp ? builder.CreateFCmpOGT(l, r)
                          : (sign ? builder.CreateICmpSGT(l, r)
                                  : builder.CreateICmpUGT(l, r));
            case BINARY_OP_GE:
                return fp ? builder.CreateFCmpOGE(l, r)
                          : (sign ? builder.CreateICmpSGE(l, r)
                                  : builder.CreateICmpUGE(l, r));
            case BINARY_OP_EQ:
                return fp ? builder.CreateFCmpOEQ(l, r) : builder.CreateICmpEQ(l, r);
            case BINARY_OP_NE:
                return fp ? builder.CreateFCmpONE(l, r) : builder.CreateICmpNE(l, r);
            default:
                unsupported("binary operator in kernel body");
        }
    }

    // ---- helpers --------------------------------------------------------

    ExpressionPtr exprChild(const std::shared_ptr<Expression>& e, size_t i) {
        if (e->getChildren().size() <= i) unsupported("missing operand");
        return std::dynamic_pointer_cast<Expression>(e->getChildren()[i]);
    }

    bool exprSigned(const ExpressionPtr& e) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(e)) {
            auto it = signedness.find(id->getTextValue());
            return it != signedness.end() ? it->second : true;
        }
        if (std::dynamic_pointer_cast<IntegerLiteralExpression>(e)) return true;
        return false;
    }

    llvm::Value* coerceTo(llvm::Value* v, llvm::Type* ty) {
        if (v->getType() == ty) return v;
        if (v->getType()->isFloatingPointTy() && ty->isFloatingPointTy())
            return builder.CreateFPCast(v, ty);
        if (v->getType()->isIntegerTy() && ty->isIntegerTy())
            return builder.CreateIntCast(v, ty, /*signed=*/true);
        return v;  // best effort; shape mismatches surface in the verifier
    }

    static std::string stripSuffix(const std::string& raw) {
        std::string s = raw;
        while (!s.empty() && (s.back() == 'f' || s.back() == 'F' ||
                              s.back() == 'd' || s.back() == 'D' ||
                              s.back() == 'l' || s.back() == 'L' ||
                              s.back() == 'u' || s.back() == 'U')) {
            s.pop_back();
        }
        return s;
    }
};

} // namespace

llvm::Function* lowerKernel(const MethodPtr& method, llvm::Module& deviceModule) {
    if (!method) unsupported("null kernel method");
    llvm::LLVMContext& ctx = deviceModule.getContext();

    // Build the device function signature. Buffer<T> / arrays become
    // addrspace(1) pointers; primitives pass by value.
    std::vector<llvm::Type*> paramTys;
    struct PInfo { std::string name; bool isSigned; llvm::Type* bufferElem; };
    std::vector<PInfo> infos;
    for (auto& p : method->getParameterList()) {
        if (!p) continue;
        if (p->getName() == "this") continue;
        CajetaTypePtr t = p->getType();
        if (isBufferType(t)) {
            llvm::Type* elem = nullptr;
            if (auto cls = std::dynamic_pointer_cast<CajetaClass>(t)) {
                if (!cls->getTypeArguments().empty()) {
                    elem = deviceScalarType(cls->getTypeArguments()[0], ctx);
                }
            }
            if (!elem) elem = llvm::Type::getFloatTy(ctx);  // default element
            paramTys.push_back(llvm::PointerType::get(ctx, kGlobalAS));
            infos.push_back({p->getName(), false, elem});
        } else {
            llvm::Type* st = deviceScalarType(t, ctx);
            if (!st) unsupported("kernel parameter type '" +
                                 (t ? t->toCanonical() : std::string("?")) + "'");
            paramTys.push_back(st);
            infos.push_back({p->getName(), typeIsSigned(t), nullptr});
        }
    }

    auto* fnTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(ctx), paramTys, /*vararg=*/false);
    auto* fn = llvm::Function::Create(
        fnTy, llvm::Function::ExternalLinkage, method->getName(), &deviceModule);
    fn->setCallingConv(llvm::CallingConv::PTX_Kernel);

    // nvvm.annotations kernel marker (belt-and-suspenders alongside the CC).
    llvm::Metadata* ops[] = {
        llvm::ValueAsMetadata::get(fn),
        llvm::MDString::get(ctx, "kernel"),
        llvm::ValueAsMetadata::get(
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 1)),
    };
    deviceModule.getOrInsertNamedMetadata("nvvm.annotations")
        ->addOperand(llvm::MDNode::get(ctx, ops));

    DeviceLowerer lowerer(deviceModule, fn);
    unsigned i = 0;
    for (auto& info : infos) {
        llvm::Argument* arg = fn->getArg(i++);
        arg->setName(info.name);
        lowerer.bindParam(info.name, arg, info.isSigned, info.bufferElem);
    }
    lowerer.lowerBody(method);
    return fn;
}

} // namespace nvidia
} // namespace xpu
} // namespace cajeta
