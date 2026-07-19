//
// NVPTX → OptiX RT-core ray-query program emission — see header.
//
// The emitted program set reproduces the M0/M1 oracle programs (test/xpu/optix/
// optix_progs.cu) and the Phase-1 spike IR (test/xpu/optix/optix_progs_ll.ll),
// parameterized by the @Kernel's launch-params layout. The `_optix_*` inline-asm
// calls are the exact ABI from the OptiX SDK's optix_device_impl.h — OptiX's module
// compiler pattern-matches them in the PTX regardless of frontend (proven on the
// 4090 in M2 Phase 1: optixModuleCreate accepts LLVM-emitted PTX).
//

#include "NvptxOptixRayQuery.h"

#include "../lowering/KernelLowering.h"   // collectKernelParamInfo, KernelParamInfo
#include "../../method/Method.h"
#include "../../asn/Statement.h"
#include "../../asn/expression/Expression.h"
#include "../../asn/expression/MethodCallExpression.h"
#include "../../asn/expression/LiteralExpression.h"
#include "../../asn/expression/Identifier.h"
#include "../../type/FormalParameter.h"
#include "cajeta/error/Exception.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"

#include <string>
#include <vector>

namespace cajeta {
namespace xpu {
namespace nvidia {

namespace {

// NVPTX address spaces: 1 = global, 4 = constant. The `params` launch block is a
// .const global (OptiX fills it via optixLaunch's pipelineParams); buffer pointers
// stored in it address global memory.
constexpr unsigned kGlobalAS = 1;
constexpr unsigned kConstAS = 4;

// Build the 49-in / 32-out `_optix_trace_typed_32` inline-asm callee (the optixTrace
// ABI). Return type is a 32×i32 struct (the payload out-registers); we read slot 0.
llvm::InlineAsm* makeTraceAsm(llvm::LLVMContext& ctx) {
    auto* i32 = llvm::Type::getInt32Ty(ctx);
    auto* i64 = llvm::Type::getInt64Ty(ctx);
    auto* f32 = llvm::Type::getFloatTy(ctx);

    std::vector<llvm::Type*> outs(32, i32);
    auto* retTy = llvm::StructType::get(ctx, outs);

    std::vector<llvm::Type*> in;
    in.push_back(i32);                 // OptixPayloadTypeID
    in.push_back(i64);                 // OptixTraversableHandle
    for (int i = 0; i < 9; ++i) in.push_back(f32);   // ox,oy,oz,dx,dy,dz,tmin,tmax,time
    for (int i = 0; i < 6; ++i) in.push_back(i32);   // mask,flags,sbtoff,sbtstride,misssbt,payloadSize
    for (int i = 0; i < 32; ++i) in.push_back(i32);  // p[1..32]
    auto* fnTy = llvm::FunctionType::get(retTy, in, /*vararg=*/false);

    std::string outsS, insS;
    for (int i = 0; i < 32; ++i) outsS += (i ? "," : "") + ("$" + std::to_string(i));
    for (int i = 32; i <= 80; ++i) insS += (i > 32 ? "," : "") + ("$" + std::to_string(i));
    std::string tmpl = "call (" + outsS + "), _optix_trace_typed_32, (" + insS + ");";

    std::vector<std::string> cons(32, "=r");
    cons.push_back("r"); cons.push_back("l");
    for (int i = 0; i < 9; ++i) cons.push_back("f");
    for (int i = 0; i < 6 + 32; ++i) cons.push_back("r");
    std::string consS;
    for (size_t i = 0; i < cons.size(); ++i) consS += (i ? "," : "") + cons[i];

    return llvm::InlineAsm::get(fnTy, tmpl, consS, /*hasSideEffects=*/true);
}

// A single-i32-result optix getter (`call ($0), <op>, ();`).
llvm::Value* callU32Getter(llvm::IRBuilder<>& b, const char* op, const char* nm) {
    auto* i32 = llvm::Type::getInt32Ty(b.getContext());
    auto* ia = llvm::InlineAsm::get(llvm::FunctionType::get(i32, false),
        std::string("call ($0), ") + op + ", ();", "=r", /*sideeffect=*/true);
    return b.CreateCall(ia, {}, nm);
}
// A u32->u32 optix getter (`call ($0), <op>, ($1);`) — e.g. the hit-kind decoders.
llvm::Value* callU32Getter1(llvm::IRBuilder<>& b, const char* op, llvm::Value* arg,
                            const char* nm) {
    auto* i32 = llvm::Type::getInt32Ty(b.getContext());
    auto* ia = llvm::InlineAsm::get(llvm::FunctionType::get(i32, {i32}, false),
        std::string("call ($0), ") + op + ", ($1);", "=r,r", /*sideeffect=*/true);
    return b.CreateCall(ia, {arg}, nm);
}
// A single-f32-result optix getter (`call ($0), <op>, ();`).
llvm::Value* callF32Getter(llvm::IRBuilder<>& b, const char* op, const char* nm) {
    auto* f32 = llvm::Type::getFloatTy(b.getContext());
    auto* ia = llvm::InlineAsm::get(llvm::FunctionType::get(f32, false),
        std::string("call ($0), ") + op + ", ();", "=f", /*sideeffect=*/true);
    return b.CreateCall(ia, {}, nm);
}
// The two-f32-result optix triangle-barycentrics getter
// (`call ($0, $1), _optix_get_triangle_barycentrics, ();`). Returns the {f,f} struct;
// the caller extracts slot 0 (u) / slot 1 (v).
llvm::Value* callBarycentrics(llvm::IRBuilder<>& b, const char* nm) {
    auto* f32 = llvm::Type::getFloatTy(b.getContext());
    auto* retTy = llvm::StructType::get(b.getContext(), {f32, f32});
    auto* ia = llvm::InlineAsm::get(llvm::FunctionType::get(retTy, false),
        "call ($0, $1), _optix_get_triangle_barycentrics, ();", "=f,=f",
        /*sideeffect=*/true);
    return b.CreateCall(ia, {}, nm);
}

llvm::Function* makeEntry(llvm::Module& m, const std::string& name) {
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(m.getContext()), false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage, name, &m);
    fn->setCallingConv(llvm::CallingConv::PTX_Kernel);   // → .entry in PTX (OptiX program)
    return fn;
}

// Recursively collect every MethodCallExpression in a method-body subtree. The
// branch/loop bodies + conditions + wrapped expressions live behind private
// accessors (NOT in getChildren), so descend them explicitly — mirrors
// Method::nodeHasStackReturn. A MethodCallExpression's args are in getParameters(),
// also off the children list. Kernel bodies are tiny, so the (harmless) double
// visit of any node that IS in children too is not a concern.
void collectCalls(const AbstractSyntaxNodePtr& node,
                  std::vector<std::shared_ptr<MethodCallExpression>>& out) {
    if (!node) return;
    if (auto mc = std::dynamic_pointer_cast<MethodCallExpression>(node)) {
        out.push_back(mc);
        for (const auto& p : mc->getParameters()) collectCalls(p.expression, out);
    }
    if (auto es = std::dynamic_pointer_cast<ExpressionStatement>(node))
        collectCalls(es->getExpression(), out);
    if (auto ret = std::dynamic_pointer_cast<ReturnStatement>(node))
        collectCalls(ret->getExpression(), out);
    if (auto lbl = std::dynamic_pointer_cast<LabelStatement>(node))
        collectCalls(lbl->getBlock(), out);
    if (auto sc = std::dynamic_pointer_cast<ScopeStatement>(node))
        collectCalls(sc->getBlock(), out);
    if (auto iff = std::dynamic_pointer_cast<IfStatement>(node)) {
        collectCalls(iff->getCondition(), out);
        collectCalls(iff->getThenBranch(), out);
        collectCalls(iff->getElseBranch(), out);
    }
    if (auto wh = std::dynamic_pointer_cast<WhileStatement>(node)) {
        collectCalls(wh->getCondition(), out);
        collectCalls(wh->getBody(), out);
    }
    if (auto fr = std::dynamic_pointer_cast<ForStatement>(node)) {
        collectCalls(fr->getCondition(), out);
        collectCalls(fr->getBody(), out);
    }
    if (auto efr = std::dynamic_pointer_cast<EnhancedForStatement>(node))
        collectCalls(efr->getBody(), out);
    if (auto dod = std::dynamic_pointer_cast<DoStatement>(node)) {
        collectCalls(dod->getCondition(), out);
        collectCalls(dod->getBody(), out);
    }
    for (const auto& c : node->getChildren()) collectCalls(c, out);
}

// Evaluate a ray-arg expression to a compile-time float (the nearest shape bakes
// its single ray's literals into raygen). Handles a float literal and a unary
// +/- over one. Returns false for anything non-constant — the caller throws
// XPU-N04 so a dynamic-ray nearest kernel falls to the software cubin, never a
// silent miscompile.
bool evalConstF32(const ExpressionPtr& e, float& out) {
    if (!e) return false;
    if (auto fl = std::dynamic_pointer_cast<FloatLiteralExpression>(e)) {
        std::string t = fl->getRawValue();
        while (!t.empty() && (t.back() == 'f' || t.back() == 'F' ||
                              t.back() == 'd' || t.back() == 'D'))
            t.pop_back();
        try { out = std::stof(t); } catch (...) { return false; }
        return true;
    }
    if (auto pe = std::dynamic_pointer_cast<PrefixExpression>(e)) {
        const auto& kids = pe->getChildren();
        if (kids.empty()) return false;
        auto operand = std::dynamic_pointer_cast<Expression>(kids[0]);
        float v;
        if (!evalConstF32(operand, v)) return false;
        if (pe->getOp() == PREFIX_OP_NEGATIVE) v = -v;
        else if (pe->getOp() != PREFIX_OP_POSITIVE) return false;
        out = v;
        return true;
    }
    return false;
}

// The single ray baked into a triangle shape's __raygen__. Extracted from the
// kernel's RayQuery.initialize(AS, rayFlags, cullMask, ox,oy,oz, tMin, dx,dy,dz, tMax)
// call — args 3..10 must be compile-time-constant floats.
struct ConstRay { float ox, oy, oz, tMin, dx, dy, dz, tMax; };
ConstRay extractConstRay(const cajeta::MethodPtr& method) {
    std::vector<std::shared_ptr<MethodCallExpression>> calls;
    if (method->getBlock()) collectCalls(method->getBlock(), calls);
    std::shared_ptr<MethodCallExpression> init;
    for (const auto& mc : calls)
        if (mc->getMethodCallName() == "initialize" &&
            mc->getParameters().size() == 11) { init = mc; break; }
    const auto throwDyn = [&]() {
        throw cajeta::Exception(
            "XPU OptiX triangle ray query (v1) requires a compile-time-constant ray "
            "in RayQuery.initialize for kernel '" + method->getName() +
            "'; force the software tier with CAJETA_GPU_AS_IMPL=software.", "XPU-N04");
    };
    if (!init) throwDyn();
    ConstRay r{};
    const auto& a = init->getParameters();
    if (!evalConstF32(a[3].expression, r.ox)  || !evalConstF32(a[4].expression, r.oy)  ||
        !evalConstF32(a[5].expression, r.oz)  || !evalConstF32(a[6].expression, r.tMin) ||
        !evalConstF32(a[7].expression, r.dx)  || !evalConstF32(a[8].expression, r.dy)  ||
        !evalConstF32(a[9].expression, r.dz)  || !evalConstF32(a[10].expression, r.tMax))
        throwDyn();
    return r;
}

// Param-kind tally for a kernel (the signature gate the shape classifier + the
// emitters share). Uses a throwaway context/layout — only kinds are read.
struct ParamTally { unsigned accel = 0, buffer = 0, scalar = 0, other = 0; };
ParamTally tallyParams(const cajeta::MethodPtr& method) {
    llvm::LLVMContext probe;
    llvm::DataLayout dl("");
    ParamTally t;
    for (const auto& pi : collectKernelParamInfo(method, probe, dl)) {
        switch (pi.kind) {
            case KernelParamInfo::AccelStruct: ++t.accel; break;
            case KernelParamInfo::Buffer:      ++t.buffer; break;
            case KernelParamInfo::Scalar:      ++t.scalar; break;
            default:                           ++t.other; break;
        }
    }
    return t;
}

} // namespace

bool nvptxKernelUsesRayQuery(const MethodPtr& method) {
    if (!method) return false;
    // collectKernelParamInfo needs a context + DataLayout for byteSizes; the kinds
    // (all we read here) are layout-independent, so a throwaway context + default
    // layout suffice for the "has an AccelerationStructure param" check.
    llvm::LLVMContext probe;
    llvm::DataLayout dl("");
    for (const auto& pi : collectKernelParamInfo(method, probe, dl))
        if (pi.kind == KernelParamInfo::AccelStruct) return true;
    return false;
}

OptixRqShape classifyRayQueryShape(const MethodPtr& method) {
    if (!nvptxKernelUsesRayQuery(method)) return OptixRqShape::Unsupported;

    std::vector<std::shared_ptr<MethodCallExpression>> calls;
    if (method && method->getBlock()) collectCalls(method->getBlock(), calls);
    bool committed = false;     // confirm / committed* — nearest-hit family
    bool candGetter = false;    // candidate distance/barycentrics — getter family
    for (const auto& mc : calls) {
        const std::string& n = mc->getMethodCallName();
        if (n == "confirmIntersection" || n == "committedType" ||
            n == "committedDistance" || n == "committedPrimitiveIndex" ||
            n == "committedBarycentricU" || n == "committedBarycentricV" ||
            n == "committedFrontFace")
            committed = true;
        else if (n == "candidateDistance" || n == "candidateBarycentricU" ||
                 n == "candidateBarycentricV")
            candGetter = true;
    }

    ParamTally t = tallyParams(method);
    if (committed) {
        // Triangle nearest-hit: (AccelerationStructure, Buffer outT, Buffer outI).
        if (t.accel == 1 && t.buffer == 2 && t.scalar == 0 && t.other == 0)
            return OptixRqShape::NearestTri;
        // Committed-triangle per-launch (kTri count / kFront front-face):
        // (AS, Buffer b0, Buffer b1, Buffer out, count). Per-launch dynamic ray.
        if (t.accel == 1 && t.buffer == 3 && t.scalar == 1 && t.other == 0)
            return OptixRqShape::CommittedTri;
        return OptixRqShape::Unsupported;
    }
    if (candGetter) {
        // Triangle candidate getters: (AccelerationStructure, Buffer<float32> out).
        if (t.accel == 1 && t.buffer == 1 && t.scalar == 0 && t.other == 0)
            return OptixRqShape::BaryCandidate;
        return OptixRqShape::Unsupported;
    }
    // AABB candidate count: (AS, Buffer originX/Y/Z, Buffer<uint32> out, count).
    if (t.accel == 1 && t.buffer == 4 && t.scalar == 1 && t.other == 0)
        return OptixRqShape::CountAabb;
    return OptixRqShape::Unsupported;
}

std::string emitOptixCountModule(const MethodPtr& method, llvm::Module& m) {
    llvm::LLVMContext& ctx = m.getContext();
    auto* i32 = llvm::Type::getInt32Ty(ctx);
    auto* i64 = llvm::Type::getInt64Ty(ctx);
    auto* f32 = llvm::Type::getFloatTy(ctx);
    auto* gptr = llvm::PointerType::get(ctx, kGlobalAS);

    // ---- recognize the canonical AABB-count signature (kind + count) -----------
    auto params = collectKernelParamInfo(method, ctx, m.getDataLayout());
    unsigned nAccel = 0, nBuffer = 0, nScalar = 0, nOther = 0;
    for (const auto& pi : params) {
        switch (pi.kind) {
            case KernelParamInfo::AccelStruct: ++nAccel; break;
            case KernelParamInfo::Buffer:      ++nBuffer; break;
            case KernelParamInfo::Scalar:      ++nScalar; break;
            default:                           ++nOther; break;   // texture/sampler/image/buffer-array
        }
    }
    if (nAccel != 1 || nBuffer != 4 || nScalar != 1 || nOther != 0) {
        throw cajeta::Exception(
            "XPU OptiX ray query (v1) supports only the canonical AABB candidate-"
            "count kernel signature: (AccelerationStructure, Buffer originX, Buffer "
            "originY, Buffer originZ, Buffer<uint32> out, count). Kernel '" +
            method->getName() + "' has a different signature; force the software "
            "tier with CAJETA_GPU_AS_IMPL=software, or await the Phase-4 shapes.",
            "XPU-N04");
    }

    const std::string kname = method->getName();

    // ---- the `params` launch block (.const) — layout per the header contract ----
    // { handle, originX, originY, originZ, out, n, boxes }
    std::vector<llvm::Type*> fields = {i64, i64, i64, i64, i64, i32, i64};
    auto* paramsTy = llvm::StructType::create(ctx, fields, "RqCountParams");
    auto* paramsG = new llvm::GlobalVariable(
        m, paramsTy, /*isConstant=*/false, llvm::GlobalValue::ExternalLinkage,
        llvm::ConstantAggregateZero::get(paramsTy), "params", nullptr,
        llvm::GlobalValue::NotThreadLocal, kConstAS);
    paramsG->setAlignment(llvm::Align(8));
    enum Field { F_HANDLE = 0, F_OX = 1, F_OY = 2, F_OZ = 3, F_OUT = 4, F_N = 5, F_BOXES = 6 };

    llvm::IRBuilder<> b(ctx);
    auto loadField = [&](unsigned idx, llvm::Type* ty) -> llvm::Value* {
        llvm::Value* p = b.CreateConstInBoundsGEP2_32(paramsTy, paramsG, 0, idx);
        return b.CreateLoad(ty, p, "p.f" + std::to_string(idx));
    };
    auto bufPtr = [&](unsigned field) -> llvm::Value* {           // params.<buf> → global ptr
        return b.CreateIntToPtr(loadField(field, i64), gptr, "buf");
    };

    auto traceAsm = makeTraceAsm(ctx);

    // ---- __raygen__<k> : per-launch-index ray, optixTrace, write count ----------
    {
        llvm::Function* fn = makeEntry(m, "__raygen__" + kname);
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
        auto* body = llvm::BasicBlock::Create(ctx, "body", fn);
        auto* ret = llvm::BasicBlock::Create(ctx, "ret", fn);
        b.SetInsertPoint(entry);
        llvm::Value* i = callU32Getter(b, "_optix_get_launch_index_x", "li");
        llvm::Value* n = loadField(F_N, i32);
        b.CreateCondBr(b.CreateICmpUGE(i, n, "oob"), ret, body);

        b.SetInsertPoint(body);
        llvm::Value* i64idx = b.CreateZExt(i, i64, "i64");
        auto loadOrigin = [&](unsigned field, const char* nm) -> llvm::Value* {
            llvm::Value* base = bufPtr(field);
            llvm::Value* ep = b.CreateInBoundsGEP(f32, base, i64idx, nm);
            return b.CreateLoad(f32, ep, nm);
        };
        llvm::Value* ox = loadOrigin(F_OX, "ox");
        llvm::Value* oy = loadOrigin(F_OY, "oy");
        llvm::Value* oz = loadOrigin(F_OZ, "oz");
        llvm::Value* handle = loadField(F_HANDLE, i64);

        std::vector<llvm::Value*> t;
        t.push_back(llvm::ConstantInt::get(i32, 0));      // type
        t.push_back(handle);
        t.push_back(ox); t.push_back(oy); t.push_back(oz);
        t.push_back(llvm::ConstantFP::get(f32, 0.0));     // dx
        t.push_back(llvm::ConstantFP::get(f32, 0.0));     // dy
        t.push_back(llvm::ConstantFP::get(f32, 1.0));     // dz
        t.push_back(llvm::ConstantFP::get(f32, 0.0));     // tmin
        t.push_back(llvm::ConstantFP::get(f32, 1.0));     // tmax (>0; point-in-box ignores t)
        t.push_back(llvm::ConstantFP::get(f32, 0.0));     // time
        t.push_back(llvm::ConstantInt::get(i32, 255));    // visibilityMask
        t.push_back(llvm::ConstantInt::get(i32, 0));      // rayFlags
        t.push_back(llvm::ConstantInt::get(i32, 0));      // SBToffset
        t.push_back(llvm::ConstantInt::get(i32, 1));      // SBTstride
        t.push_back(llvm::ConstantInt::get(i32, 0));      // missSBTIndex
        t.push_back(llvm::ConstantInt::get(i32, 1));      // payloadSize
        for (int k = 0; k < 32; ++k) t.push_back(llvm::ConstantInt::get(i32, 0));  // p[1..32]
        llvm::Value* tr = b.CreateCall(traceAsm, t, "trace");
        llvm::Value* count = b.CreateExtractValue(tr, 0, "count");

        llvm::Value* outBase = bufPtr(F_OUT);
        llvm::Value* outEp = b.CreateInBoundsGEP(i32, outBase, i64idx, "out.ep");
        b.CreateStore(count, outEp);
        b.CreateBr(ret);

        b.SetInsertPoint(ret);
        b.CreateRetVoid();
    }

    // ---- __intersection__<k> : point-in-box against params.boxes ----------------
    {
        llvm::Function* fn = makeEntry(m, "__intersection__" + kname);
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
        auto* report = llvm::BasicBlock::Create(ctx, "report", fn);
        auto* ret = llvm::BasicBlock::Create(ctx, "ret", fn);
        b.SetInsertPoint(entry);
        llvm::Value* idx = callU32Getter(b, "_optix_read_primitive_idx", "prim");
        llvm::Value* ox = callF32Getter(b, "_optix_get_object_ray_origin_x", "ox");
        llvm::Value* oy = callF32Getter(b, "_optix_get_object_ray_origin_y", "oy");
        llvm::Value* oz = callF32Getter(b, "_optix_get_object_ray_origin_z", "oz");
        llvm::Value* bbase = bufPtr(F_BOXES);
        llvm::Value* base6 = b.CreateMul(b.CreateZExt(idx, i64, "idx64"),
                                         llvm::ConstantInt::get(i64, 6), "base6");
        auto box = [&](int k) -> llvm::Value* {
            llvm::Value* off = b.CreateAdd(base6, llvm::ConstantInt::get(i64, k));
            return b.CreateLoad(f32, b.CreateInBoundsGEP(f32, bbase, off), "b");
        };
        llvm::Value* inside = b.CreateAnd(
            b.CreateAnd(b.CreateAnd(b.CreateFCmpOGE(ox, box(0)),
                                    b.CreateFCmpOGE(oy, box(1))),
                        b.CreateAnd(b.CreateFCmpOGE(oz, box(2)),
                                    b.CreateFCmpOLE(ox, box(3)))),
            b.CreateAnd(b.CreateFCmpOLE(oy, box(4)), b.CreateFCmpOLE(oz, box(5))),
            "inside");
        b.CreateCondBr(inside, report, ret);

        b.SetInsertPoint(report);
        auto* repTy = llvm::FunctionType::get(i32, {f32, i32}, false);
        auto* repAsm = llvm::InlineAsm::get(
            repTy, "call ($0), _optix_report_intersection_0, ($1, $2);", "=r,f,r",
            /*sideeffect=*/true);
        b.CreateCall(repAsm, {llvm::ConstantFP::get(f32, 0.0), llvm::ConstantInt::get(i32, 0)});
        b.CreateBr(ret);

        b.SetInsertPoint(ret);
        b.CreateRetVoid();
    }

    // ---- __anyhit__<k> : count this candidate, keep traversing ------------------
    {
        llvm::Function* fn = makeEntry(m, "__anyhit__" + kname);
        b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", fn));
        auto* getTy = llvm::FunctionType::get(i32, {i32}, false);
        auto* getAsm = llvm::InlineAsm::get(
            getTy, "call ($0), _optix_get_payload, ($1);", "=r,r", true);
        llvm::Value* p = b.CreateCall(getAsm, {llvm::ConstantInt::get(i32, 0)}, "p");
        llvm::Value* p1 = b.CreateAdd(p, llvm::ConstantInt::get(i32, 1), "p1");
        auto* setTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {i32, i32}, false);
        auto* setAsm = llvm::InlineAsm::get(
            setTy, "call _optix_set_payload, ($0, $1);", "r,r", true);
        b.CreateCall(setAsm, {llvm::ConstantInt::get(i32, 0), p1});
        auto* ignTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), false);
        auto* ignAsm = llvm::InlineAsm::get(
            ignTy, "call _optix_ignore_intersection, ();", "", true);
        b.CreateCall(ignAsm, {});
        b.CreateRetVoid();
    }

    // ---- __miss__<k> : nothing to count ----------------------------------------
    {
        llvm::Function* fn = makeEntry(m, "__miss__" + kname);
        b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", fn));
        b.CreateRetVoid();
    }

    return "__raygen__" + kname;
}

std::string emitOptixNearestModule(const MethodPtr& method, llvm::Module& m) {
    llvm::LLVMContext& ctx = m.getContext();
    auto* i32 = llvm::Type::getInt32Ty(ctx);
    auto* i64 = llvm::Type::getInt64Ty(ctx);
    auto* f32 = llvm::Type::getFloatTy(ctx);
    auto* gptr = llvm::PointerType::get(ctx, kGlobalAS);

    // ---- recognize the canonical triangle nearest-hit shape --------------------
    if (classifyRayQueryShape(method) != OptixRqShape::NearestTri) {
        throw cajeta::Exception(
            "XPU OptiX ray query (v1) nearest-hit supports only the canonical "
            "triangle shape: (AccelerationStructure, Buffer<float32> outT, "
            "Buffer<uint32> outI) whose body confirms triangle candidates and reads "
            "committed getters, with a compile-time-constant ray. Kernel '" +
            method->getName() + "' does not match; force the software tier with "
            "CAJETA_GPU_AS_IMPL=software.",
            "XPU-N04");
    }

    // ---- extract the single ray from the initialize() literals -----------------
    ConstRay ray = extractConstRay(method);
    const float ox = ray.ox, oy = ray.oy, oz = ray.oz, tMin = ray.tMin;
    const float dx = ray.dx, dy = ray.dy, dz = ray.dz, tMax = ray.tMax;

    const std::string kname = method->getName();

    // ---- the `params` launch block (.const) — { handle, outT, outI } -----------
    std::vector<llvm::Type*> fields = {i64, i64, i64};
    auto* paramsTy = llvm::StructType::create(ctx, fields, "RqNearestParams");
    auto* paramsG = new llvm::GlobalVariable(
        m, paramsTy, /*isConstant=*/false, llvm::GlobalValue::ExternalLinkage,
        llvm::ConstantAggregateZero::get(paramsTy), "params", nullptr,
        llvm::GlobalValue::NotThreadLocal, kConstAS);
    paramsG->setAlignment(llvm::Align(8));
    enum Field { F_HANDLE = 0, F_OUTT = 1, F_OUTI = 2 };

    llvm::IRBuilder<> b(ctx);
    auto loadField = [&](unsigned idx, llvm::Type* ty) -> llvm::Value* {
        llvm::Value* p = b.CreateConstInBoundsGEP2_32(paramsTy, paramsG, 0, idx);
        return b.CreateLoad(ty, p, "p.f" + std::to_string(idx));
    };
    auto bufPtr = [&](unsigned field) -> llvm::Value* {
        return b.CreateIntToPtr(loadField(field, i64), gptr, "buf");
    };

    auto traceAsm = makeTraceAsm(ctx);

    // ---- __raygen__<k> : single baked ray, built-in triangle traversal ---------
    {
        llvm::Function* fn = makeEntry(m, "__raygen__" + kname);
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
        auto* body = llvm::BasicBlock::Create(ctx, "body", fn);
        auto* ret = llvm::BasicBlock::Create(ctx, "ret", fn);
        b.SetInsertPoint(entry);
        llvm::Value* i = callU32Getter(b, "_optix_get_launch_index_x", "li");
        b.CreateCondBr(b.CreateICmpNE(i, llvm::ConstantInt::get(i32, 0), "nz"),
                       ret, body);

        b.SetInsertPoint(body);
        llvm::Value* handle = loadField(F_HANDLE, i64);
        std::vector<llvm::Value*> t;
        t.push_back(llvm::ConstantInt::get(i32, 0));      // payload type
        t.push_back(handle);
        t.push_back(llvm::ConstantFP::get(f32, ox)); t.push_back(llvm::ConstantFP::get(f32, oy));
        t.push_back(llvm::ConstantFP::get(f32, oz));
        t.push_back(llvm::ConstantFP::get(f32, dx)); t.push_back(llvm::ConstantFP::get(f32, dy));
        t.push_back(llvm::ConstantFP::get(f32, dz));
        t.push_back(llvm::ConstantFP::get(f32, tMin)); t.push_back(llvm::ConstantFP::get(f32, tMax));
        t.push_back(llvm::ConstantFP::get(f32, 0.0));     // time
        t.push_back(llvm::ConstantInt::get(i32, 255));    // visibilityMask
        t.push_back(llvm::ConstantInt::get(i32, 0));      // rayFlags
        t.push_back(llvm::ConstantInt::get(i32, 0));      // SBToffset
        t.push_back(llvm::ConstantInt::get(i32, 1));      // SBTstride
        t.push_back(llvm::ConstantInt::get(i32, 0));      // missSBTIndex
        t.push_back(llvm::ConstantInt::get(i32, 0));      // payloadSize (closesthit writes via params)
        for (int k = 0; k < 32; ++k) t.push_back(llvm::ConstantInt::get(i32, 0));
        b.CreateCall(traceAsm, t, "trace");
        b.CreateBr(ret);

        b.SetInsertPoint(ret);
        b.CreateRetVoid();
    }

    // ---- __closesthit__<k> : commit nearest T / type=triangle / prim -----------
    {
        llvm::Function* fn = makeEntry(m, "__closesthit__" + kname);
        b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", fn));
        llvm::Value* tmax = callF32Getter(b, "_optix_get_ray_tmax", "t");
        llvm::Value* prim = callU32Getter(b, "_optix_read_primitive_idx", "prim");
        llvm::Value* outT = bufPtr(F_OUTT);
        llvm::Value* outI = bufPtr(F_OUTI);
        b.CreateStore(tmax, b.CreateInBoundsGEP(f32, outT,
                          llvm::ConstantInt::get(i64, 0), "outT.0"));
        // committedType triangle = 1, committedPrimitiveIndex at outI[1].
        b.CreateStore(llvm::ConstantInt::get(i32, 1),
                      b.CreateInBoundsGEP(i32, outI, llvm::ConstantInt::get(i64, 0), "outI.0"));
        b.CreateStore(prim,
                      b.CreateInBoundsGEP(i32, outI, llvm::ConstantInt::get(i64, 1), "outI.1"));
        b.CreateRetVoid();
    }

    // ---- __miss__<k> : committed type NONE = 0 ---------------------------------
    {
        llvm::Function* fn = makeEntry(m, "__miss__" + kname);
        b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", fn));
        llvm::Value* outI = bufPtr(F_OUTI);
        b.CreateStore(llvm::ConstantInt::get(i32, 0),
                      b.CreateInBoundsGEP(i32, outI, llvm::ConstantInt::get(i64, 0), "outI.0"));
        b.CreateRetVoid();
    }

    return "__raygen__" + kname;
}

std::string emitOptixBaryModule(const MethodPtr& method, llvm::Module& m) {
    llvm::LLVMContext& ctx = m.getContext();
    auto* i32 = llvm::Type::getInt32Ty(ctx);
    auto* i64 = llvm::Type::getInt64Ty(ctx);
    auto* f32 = llvm::Type::getFloatTy(ctx);
    auto* gptr = llvm::PointerType::get(ctx, kGlobalAS);

    // ---- recognize the canonical triangle candidate-getter shape ---------------
    if (classifyRayQueryShape(method) != OptixRqShape::BaryCandidate) {
        throw cajeta::Exception(
            "XPU OptiX ray query (v1) candidate getters supports only the canonical "
            "shape: (AccelerationStructure, Buffer<float32> out) whose body reads "
            "candidate distance/barycentric getters, with a compile-time-constant "
            "ray. Kernel '" + method->getName() + "' does not match; force the "
            "software tier with CAJETA_GPU_AS_IMPL=software.", "XPU-N04");
    }

    // ---- extract the single ray from the initialize() literals -----------------
    ConstRay ray = extractConstRay(method);

    const std::string kname = method->getName();

    // ---- the `params` launch block (.const) — { handle, out } ------------------
    std::vector<llvm::Type*> fields = {i64, i64};
    auto* paramsTy = llvm::StructType::create(ctx, fields, "RqBaryParams");
    auto* paramsG = new llvm::GlobalVariable(
        m, paramsTy, /*isConstant=*/false, llvm::GlobalValue::ExternalLinkage,
        llvm::ConstantAggregateZero::get(paramsTy), "params", nullptr,
        llvm::GlobalValue::NotThreadLocal, kConstAS);
    paramsG->setAlignment(llvm::Align(8));
    enum Field { F_HANDLE = 0, F_OUT = 1 };

    llvm::IRBuilder<> b(ctx);
    auto loadField = [&](unsigned idx, llvm::Type* ty) -> llvm::Value* {
        llvm::Value* p = b.CreateConstInBoundsGEP2_32(paramsTy, paramsG, 0, idx);
        return b.CreateLoad(ty, p, "p.f" + std::to_string(idx));
    };
    auto bufPtr = [&](unsigned field) -> llvm::Value* {
        return b.CreateIntToPtr(loadField(field, i64), gptr, "buf");
    };

    auto traceAsm = makeTraceAsm(ctx);

    // ---- __raygen__<k> : single baked ray, built-in triangle traversal ---------
    {
        llvm::Function* fn = makeEntry(m, "__raygen__" + kname);
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
        auto* body = llvm::BasicBlock::Create(ctx, "body", fn);
        auto* ret = llvm::BasicBlock::Create(ctx, "ret", fn);
        b.SetInsertPoint(entry);
        llvm::Value* i = callU32Getter(b, "_optix_get_launch_index_x", "li");
        b.CreateCondBr(b.CreateICmpNE(i, llvm::ConstantInt::get(i32, 0), "nz"),
                       ret, body);

        b.SetInsertPoint(body);
        llvm::Value* handle = loadField(F_HANDLE, i64);
        std::vector<llvm::Value*> t;
        t.push_back(llvm::ConstantInt::get(i32, 0));      // payload type
        t.push_back(handle);
        t.push_back(llvm::ConstantFP::get(f32, ray.ox)); t.push_back(llvm::ConstantFP::get(f32, ray.oy));
        t.push_back(llvm::ConstantFP::get(f32, ray.oz));
        t.push_back(llvm::ConstantFP::get(f32, ray.dx)); t.push_back(llvm::ConstantFP::get(f32, ray.dy));
        t.push_back(llvm::ConstantFP::get(f32, ray.dz));
        t.push_back(llvm::ConstantFP::get(f32, ray.tMin)); t.push_back(llvm::ConstantFP::get(f32, ray.tMax));
        t.push_back(llvm::ConstantFP::get(f32, 0.0));     // time
        t.push_back(llvm::ConstantInt::get(i32, 255));    // visibilityMask
        t.push_back(llvm::ConstantInt::get(i32, 0));      // rayFlags
        t.push_back(llvm::ConstantInt::get(i32, 0));      // SBToffset
        t.push_back(llvm::ConstantInt::get(i32, 1));      // SBTstride
        t.push_back(llvm::ConstantInt::get(i32, 0));      // missSBTIndex
        t.push_back(llvm::ConstantInt::get(i32, 0));      // payloadSize (anyhit writes via params)
        for (int k = 0; k < 32; ++k) t.push_back(llvm::ConstantInt::get(i32, 0));
        b.CreateCall(traceAsm, t, "trace");
        b.CreateBr(ret);

        b.SetInsertPoint(ret);
        b.CreateRetVoid();
    }

    // ---- __anyhit__<k> : read the candidate's t + barycentrics, keep traversing -
    {
        llvm::Function* fn = makeEntry(m, "__anyhit__" + kname);
        b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", fn));
        llvm::Value* tcand = callF32Getter(b, "_optix_get_ray_tmax", "t");
        llvm::Value* bary = callBarycentrics(b, "bary");
        llvm::Value* u = b.CreateExtractValue(bary, 0, "u");
        llvm::Value* v = b.CreateExtractValue(bary, 1, "v");
        llvm::Value* out = bufPtr(F_OUT);
        b.CreateStore(tcand, b.CreateInBoundsGEP(f32, out,
                          llvm::ConstantInt::get(i64, 0), "out.0"));
        b.CreateStore(u, b.CreateInBoundsGEP(f32, out,
                          llvm::ConstantInt::get(i64, 1), "out.1"));
        b.CreateStore(v, b.CreateInBoundsGEP(f32, out,
                          llvm::ConstantInt::get(i64, 2), "out.2"));
        // optixIgnoreIntersection — never commit; faithful candidate enumeration.
        auto* ignTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), false);
        auto* ignAsm = llvm::InlineAsm::get(
            ignTy, "call _optix_ignore_intersection, ();", "", true);
        b.CreateCall(ignAsm, {});
        b.CreateRetVoid();
    }

    // ---- __miss__<k> : nothing (the ray hits; out retains anyhit's writes) ------
    {
        llvm::Function* fn = makeEntry(m, "__miss__" + kname);
        b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", fn));
        b.CreateRetVoid();
    }

    return "__raygen__" + kname;
}

std::string emitOptixCommittedTriModule(const MethodPtr& method, llvm::Module& m) {
    llvm::LLVMContext& ctx = m.getContext();
    auto* i32 = llvm::Type::getInt32Ty(ctx);
    auto* i64 = llvm::Type::getInt64Ty(ctx);
    auto* f32 = llvm::Type::getFloatTy(ctx);
    auto* gptr = llvm::PointerType::get(ctx, kGlobalAS);

    const auto throwN04 = [&](const std::string& why) {
        throw cajeta::Exception(
            "XPU OptiX ray query (v1) committed-triangle shape: " + why + " Kernel '" +
            method->getName() + "' does not match the canonical (AccelerationStructure, "
            "Buffer b0, Buffer b1, Buffer out, count) per-launch shape with a "
            "const-or-buffer[i] ray; force the software tier with "
            "CAJETA_GPU_AS_IMPL=software.", "XPU-N04");
    };
    if (classifyRayQueryShape(method) != OptixRqShape::CommittedTri)
        throwN04("signature/body mismatch.");

    // ---- buffer param names in signature order (b0, b1, out) -------------------
    // collectKernelParamInfo (= collectParams) skips an implicit `this`, so filter it
    // here too to keep the index-zip exact (the @Kernel is static, but be robust).
    auto info = collectKernelParamInfo(method, ctx, m.getDataLayout());
    std::vector<std::string> formal;
    for (auto& p : method->getParameterList())
        if (p && p->getName() != "this") formal.push_back(p->getName());
    std::vector<std::string> bufNames;            // Buffer params, in signature order
    for (size_t i = 0; i < info.size() && i < formal.size(); ++i)
        if (info[i].kind == KernelParamInfo::Buffer)
            bufNames.push_back(formal[i]);
    if (bufNames.size() != 3) throwN04("expected exactly 3 Buffer params.");
    enum Field { F_HANDLE = 0, F_B0 = 1, F_B1 = 2, F_OUT = 3, F_N = 4 };

    // ---- resolve each ray component: a constant, or a b0[i]/b1[i] load ----------
    // RayComp.field == -1 means a constant `value`; else it's F_B0/F_B1 (load[i]).
    struct RayComp { int field = -1; float value = 0.0f; };
    // The raygen loads every b0[...]/b1[...] component at the launch index, so
    // the subscript must be exactly the launch-index variable: a bare
    // identifier, identical across all components. A computed index (b0[i+1],
    // b0[2*i]) or a second, differing index would otherwise be silently loaded
    // at the launch index instead — the XPU-N04 "never a silent miscompile"
    // invariant. Pin the shared index name here.
    std::string rayIndexName;
    const auto resolveComp = [&](const ExpressionPtr& e) -> RayComp {
        RayComp rc;
        if (evalConstF32(e, rc.value)) return rc;
        // A `name[i]` load: ArrayIndexExpression whose base identifier is b0 or b1.
        if (auto ai = std::dynamic_pointer_cast<ArrayIndexExpression>(e)) {
            const auto& kids = ai->getChildren();
            if (kids.size() >= 2) {
                if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(kids[0])) {
                    const std::string& nm = id->getTextValue();
                    if (nm == bufNames[0] || nm == bufNames[1]) {
                        auto idxId = std::dynamic_pointer_cast<IdentifierExpression>(kids[1]);
                        if (!idxId)
                            throwN04("a ray-component index must be the plain "
                                     "launch-index variable, not a computed expression.");
                        if (rayIndexName.empty())
                            rayIndexName = idxId->getTextValue();
                        else if (rayIndexName != idxId->getTextValue())
                            throwN04("ray components use differing index variables.");
                        rc.field = (nm == bufNames[0]) ? F_B0 : F_B1;
                        return rc;
                    }
                }
            }
        }
        throwN04("a ray component is neither a constant nor a b0[i]/b1[i] load.");
        return rc;  // unreachable
    };

    std::vector<std::shared_ptr<MethodCallExpression>> calls;
    if (method->getBlock()) collectCalls(method->getBlock(), calls);
    std::shared_ptr<MethodCallExpression> init;
    bool frontFace = false;
    for (const auto& mc : calls) {
        if (mc->getMethodCallName() == "initialize" && mc->getParameters().size() == 11)
            init = mc;
        if (mc->getMethodCallName() == "committedFrontFace") frontFace = true;
    }
    if (!init) throwN04("no initialize(...) call.");
    const auto& a = init->getParameters();
    // args[3..10] = ox, oy, oz, tMin, dx, dy, dz, tMax.
    RayComp ox = resolveComp(a[3].expression),  oy = resolveComp(a[4].expression);
    RayComp oz = resolveComp(a[5].expression),  tMin = resolveComp(a[6].expression);
    RayComp dx = resolveComp(a[7].expression),  dy = resolveComp(a[8].expression);
    RayComp dz = resolveComp(a[9].expression),  tMax = resolveComp(a[10].expression);

    const std::string kname = method->getName();

    // ---- the `params` launch block (.const) — { handle, b0, b1, out, n } -------
    std::vector<llvm::Type*> fields = {i64, i64, i64, i64, i32};
    auto* paramsTy = llvm::StructType::create(ctx, fields, "RqCommittedTriParams");
    auto* paramsG = new llvm::GlobalVariable(
        m, paramsTy, /*isConstant=*/false, llvm::GlobalValue::ExternalLinkage,
        llvm::ConstantAggregateZero::get(paramsTy), "params", nullptr,
        llvm::GlobalValue::NotThreadLocal, kConstAS);
    paramsG->setAlignment(llvm::Align(8));

    llvm::IRBuilder<> b(ctx);
    auto loadField = [&](unsigned idx, llvm::Type* ty) -> llvm::Value* {
        llvm::Value* p = b.CreateConstInBoundsGEP2_32(paramsTy, paramsG, 0, idx);
        return b.CreateLoad(ty, p, "p.f" + std::to_string(idx));
    };
    auto bufPtr = [&](unsigned field) -> llvm::Value* {
        return b.CreateIntToPtr(loadField(field, i64), gptr, "buf");
    };
    auto traceAsm = makeTraceAsm(ctx);

    // ---- __raygen__<k> : per-launch index, resolved ray, triangle traversal -----
    {
        llvm::Function* fn = makeEntry(m, "__raygen__" + kname);
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
        auto* body = llvm::BasicBlock::Create(ctx, "body", fn);
        auto* ret = llvm::BasicBlock::Create(ctx, "ret", fn);
        b.SetInsertPoint(entry);
        llvm::Value* i = callU32Getter(b, "_optix_get_launch_index_x", "li");
        llvm::Value* n = loadField(F_N, i32);
        b.CreateCondBr(b.CreateICmpUGE(i, n, "oob"), ret, body);

        b.SetInsertPoint(body);
        llvm::Value* i64idx = b.CreateZExt(i, i64, "i64");
        // Materialize a ray component: constant, or a per-launch load from its buffer.
        auto comp = [&](const RayComp& rc, const char* nm) -> llvm::Value* {
            if (rc.field < 0) return llvm::ConstantFP::get(f32, rc.value);
            llvm::Value* base = bufPtr((unsigned) rc.field);
            llvm::Value* ep = b.CreateInBoundsGEP(f32, base, i64idx, nm);
            return b.CreateLoad(f32, ep, nm);
        };
        llvm::Value* handle = loadField(F_HANDLE, i64);
        std::vector<llvm::Value*> t;
        t.push_back(llvm::ConstantInt::get(i32, 0));      // payload type
        t.push_back(handle);
        t.push_back(comp(ox, "ox")); t.push_back(comp(oy, "oy")); t.push_back(comp(oz, "oz"));
        t.push_back(comp(dx, "dx")); t.push_back(comp(dy, "dy")); t.push_back(comp(dz, "dz"));
        t.push_back(comp(tMin, "tmin")); t.push_back(comp(tMax, "tmax"));
        t.push_back(llvm::ConstantFP::get(f32, 0.0));     // time
        t.push_back(llvm::ConstantInt::get(i32, 255));    // visibilityMask
        t.push_back(llvm::ConstantInt::get(i32, 0));      // rayFlags
        t.push_back(llvm::ConstantInt::get(i32, 0));      // SBToffset
        t.push_back(llvm::ConstantInt::get(i32, 1));      // SBTstride
        t.push_back(llvm::ConstantInt::get(i32, 0));      // missSBTIndex
        t.push_back(llvm::ConstantInt::get(i32, 0));      // payloadSize (closesthit via params)
        for (int k = 0; k < 32; ++k) t.push_back(llvm::ConstantInt::get(i32, 0));
        b.CreateCall(traceAsm, t, "trace");
        b.CreateBr(ret);

        b.SetInsertPoint(ret);
        b.CreateRetVoid();
    }

    // ---- __closesthit__<k> : write out[i] (hit-flag, or front-face 1/2) ---------
    {
        llvm::Function* fn = makeEntry(m, "__closesthit__" + kname);
        b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", fn));
        llvm::Value* i = callU32Getter(b, "_optix_get_launch_index_x", "li");
        llvm::Value* i64idx = b.CreateZExt(i, i64, "i64");
        llvm::Value* out = bufPtr(F_OUT);
        llvm::Value* val;
        if (frontFace) {
            // optixIsFrontFaceHit = !(backface(hitKind) == 1) -> 1 front / 2 back.
            llvm::Value* hk = callU32Getter(b, "_optix_get_hit_kind", "hk");
            llvm::Value* bf = callU32Getter1(b, "_optix_get_backface_from_hit_kind", hk, "bf");
            llvm::Value* isBack = b.CreateICmpEQ(bf, llvm::ConstantInt::get(i32, 1), "isBack");
            val = b.CreateSelect(isBack, llvm::ConstantInt::get(i32, 2),
                                 llvm::ConstantInt::get(i32, 1), "ff");
        } else {
            val = llvm::ConstantInt::get(i32, 1);          // hit-flag: committed type triangle
        }
        b.CreateStore(val, b.CreateInBoundsGEP(i32, out, i64idx, "out.i"));
        b.CreateRetVoid();
    }

    // ---- __miss__<k> : out[i] = 0 (no committed hit) ----------------------------
    {
        llvm::Function* fn = makeEntry(m, "__miss__" + kname);
        b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", fn));
        llvm::Value* i = callU32Getter(b, "_optix_get_launch_index_x", "li");
        llvm::Value* i64idx = b.CreateZExt(i, i64, "i64");
        llvm::Value* out = bufPtr(F_OUT);
        b.CreateStore(llvm::ConstantInt::get(i32, 0),
                      b.CreateInBoundsGEP(i32, out, i64idx, "out.i"));
        b.CreateRetVoid();
    }

    return "__raygen__" + kname;
}

} // namespace nvidia
} // namespace xpu
} // namespace cajeta
