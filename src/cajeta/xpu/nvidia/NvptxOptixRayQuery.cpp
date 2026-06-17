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
// A single-f32-result optix getter (`call ($0), <op>, ();`).
llvm::Value* callF32Getter(llvm::IRBuilder<>& b, const char* op, const char* nm) {
    auto* f32 = llvm::Type::getFloatTy(b.getContext());
    auto* ia = llvm::InlineAsm::get(llvm::FunctionType::get(f32, false),
        std::string("call ($0), ") + op + ", ();", "=f", /*sideeffect=*/true);
    return b.CreateCall(ia, {}, nm);
}

llvm::Function* makeEntry(llvm::Module& m, const std::string& name) {
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(m.getContext()), false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage, name, &m);
    fn->setCallingConv(llvm::CallingConv::PTX_Kernel);   // → .entry in PTX (OptiX program)
    return fn;
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

} // namespace nvidia
} // namespace xpu
} // namespace cajeta
