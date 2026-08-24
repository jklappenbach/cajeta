//
// CPU kernel lowering — see header. The host LoweringTarget + thin wrapper.
//

#include "CpuKernelLowering.h"

#include "../lowering/KernelLowering.h"
#include "../lowering/LoweringTarget.h"
#include "../../error/Exception.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/ModRef.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "CpuBackend.h"
#include "../../type/VectorOps.h"
#include <cstdlib>
#include <memory>

namespace cajeta {
namespace xpu {
namespace cpu {

namespace {

// The CPU LoweringTarget. The SIMT backends read coordinates from hardware
// intrinsics; the CPU has none, so the grid→threads model passes a work-item's
// coordinates as the 12 trailing i32 kernel args and these reads pull them back.
class CpuTarget : public LoweringTarget {
public:
    const char* name() const override { return "cpu"; }

    // Wide `dotAccum` — the CPU backend's ISA IS the host's, so a @Kernel
    // must reach the same tier an ordinary method does. It did not: the
    // per-lane `integerDot4x8` seam handed x86 four int8 lanes at a time,
    // and `vpdpbusd` needs sixteen, so every kernel dotAccum fell to the
    // portable widening reduce. Measured, in one binary: the host form of a
    // Q4_K/q8_K mat-vec got 8 `vpdpbusd` and the identical @Kernel got 8
    // `vpmaddwd`.
    //
    // The subtarget is asked the same way `createCpuTargetMachine` builds
    // it — host CPU plus its native features — because that is precisely
    // what the CPU backend compiles kernels for. Asking anything else would
    // let this disagree with the machine the object actually targets.
    llvm::Value* integerDotWide(llvm::IRBuilderBase& b, llvm::Module& m,
                                llvm::Value* w, llvm::Value* a,
                                llvm::Value* acc, bool wUnsigned) override {
        const char* fsd = std::getenv("CAJETA_SIMD_SCALAR_FALLBACK");
        if (fsd && fsd[0] && fsd[0] != '0') return nullptr;   // stays a control
        static const std::unique_ptr<llvm::TargetMachine> tm =
            createCpuTargetMachine();
        if (!tm || !tm->getTargetTriple().isX86()) return nullptr;
        const llvm::MCSubtargetInfo& sti = tm->getMCSubtargetInfo();
        if (!sti.checkFeatures("+avx512vnni") && !sti.checkFeatures("+avxvnni"))
            return nullptr;

        // VNNI ONLY, and the shape checked here rather than left to
        // vecops::dotAccum's tier order.
        //
        // The reason is a correctness one, not caution. `vecops::dotAccum`
        // prefers a three-instruction AVX2 tier over its exact one when VNNI
        // is absent, and that tier SATURATES — authorized on the host, where
        // every caller is a K-quant with headroom to spare. The CPU backend
        // is not just another caller: it is the bit-exact oracle the GPU
        // backends are checked against, so it must not silently acquire a
        // saturating lowering on an AVX2-only machine. `vpdpbusd` does not
        // saturate, so taking that tier and only that tier changes speed and
        // never results.
        auto* wt = llvm::dyn_cast<llvm::FixedVectorType>(w->getType());
        auto* at = llvm::dyn_cast<llvm::FixedVectorType>(acc->getType());
        if (!wUnsigned || wt == nullptr || at == nullptr) return nullptr;
        unsigned n = at->getNumElements();
        if (wt->getNumElements() != n * 4) return nullptr;
        if (n != 4 && n != 8 && n != 16) return nullptr;

        vecops::DotAccumTargets t;
        t.vnni = true;
        t.avx2 = false;             // never reachable given the gate above
        t.forceScalar = false;
        return vecops::dotAccum(b, &m, w, a, acc, wUnsigned, t);
    }

    // No native inline ray query: the AccelerationStructure noun is built as the
    // portable software BVH, so the RayQuery verb follows to the SoftwareRayQuery
    // walk over a software BVH buffer (ray-query-to-core inc 1). softwareRayQuery()
    // derives from this in the base.
    NounImpl accelImpl() const override { return NounImpl::SoftwareBvh; }

    // Flat host address space.
    unsigned allocaAddressSpace() const override { return 0; }

    llvm::Value* threadId(llvm::IRBuilderBase& b, llvm::Module&,
                          unsigned dim) override {
        return coord(b, /*group=*/0, dim);     // tid.{x,y,z}
    }
    llvm::Value* workgroupId(llvm::IRBuilderBase& b, llvm::Module&,
                             unsigned dim) override {
        return coord(b, /*group=*/3, dim);     // ctaid.{x,y,z}
    }
    llvm::Value* workgroupDim(llvm::IRBuilderBase& b, llvm::Module&,
                              unsigned dim) override {
        return coord(b, /*group=*/6, dim);     // ntid.{x,y,z}
    }
    // globalId uses the shared default: ctaid*ntid + tid.

    // Grid-stride stride (Item 6 Stage 2). Total work-items in `dim` =
    // gridDim·blockDim = nctaid·ntid. The SIMT backends read these from hardware;
    // the CPU now carries the 4th coord group, gridDim (nctaid = block count),
    // threaded from the launch ABI (runtime → thunk → wrapper → kernel). Returns
    // i32, matching the other coord reads.
    llvm::Value* gridSize(llvm::IRBuilderBase& b, llvm::Module&,
                          unsigned dim) override {
        return b.CreateMul(coord(b, /*group=*/9, dim),   // nctaid.{x,y,z}
                           coord(b, /*group=*/6, dim),   // ntid.{x,y,z}
                           "xpu.gridsize");
    }

    void workgroupBarrier(llvm::IRBuilderBase& b, llvm::Module& m) override {
        // A CPU workgroup barrier is realized by work-item loop fission in the
        // registration pass (cajeta-cpu.md Inc 6): this marker call delimits the
        // regions the fission pass splits the work-item loop at. It is left
        // impure (default memory effects), noinline, and noduplicate so the
        // optimizer neither deletes nor clones/moves it before fission runs; the
        // pass erases every call once it has split the regions.
        llvm::LLVMContext& ctx = m.getContext();
        auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx),
                                             /*vararg=*/false);
        llvm::FunctionCallee callee =
            m.getOrInsertFunction("__cajeta_xpu_cpu_barrier", fnTy);
        if (auto* f = llvm::dyn_cast<llvm::Function>(callee.getCallee())) {
            f->addFnAttr(llvm::Attribute::NoInline);
            f->addFnAttr(llvm::Attribute::NoDuplicate);
            f->setDoesNotThrow();
        }
        b.CreateCall(callee, {});
    }

    void devicePrintf(llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* fmt,
                      llvm::ArrayRef<llvm::Value*> args) override {
        // The CPU kernel runs as host code via LLJIT, so a direct call to libc
        // `printf` works. Promote each f32 to double (C's default varargs
        // promotion — `%f` reads a double); ints pass through.
        llvm::LLVMContext& ctx = m.getContext();
        auto* i32 = llvm::Type::getInt32Ty(ctx);
        auto* ptr = llvm::PointerType::get(ctx, 0);
        auto* fnTy = llvm::FunctionType::get(i32, {ptr}, /*vararg=*/true);
        llvm::FunctionCallee pf = m.getOrInsertFunction("printf", fnTy);
        std::vector<llvm::Value*> call;
        call.reserve(args.size() + 1);
        call.push_back(fmt);
        for (llvm::Value* a : args) {
            if (a->getType()->isFloatTy())
                a = b.CreateFPExt(a, llvm::Type::getDoubleTy(ctx), "printf.f2d");
            call.push_back(a);
        }
        b.CreateCall(pf, call);
    }

    // Specialization constants (Stage 11/12, hybrid host-override): the default
    // LoweringTarget bakes the literal, but on CPU we read the value at runtime
    // so a host `spec:[...]` override is honored without a per-value recompile
    // (folding is irrelevant on the oracle path). Emit a call to the runtime
    // helper, which returns the override for `slot` if the launch supplied one,
    // else `defaultValue`. No override → the helper returns the default (today's
    // observable result), so behavior is unchanged when nothing is passed.
    llvm::Value* specConstantI32(llvm::IRBuilderBase& b, llvm::Module& m,
                                 unsigned slot, int32_t defaultValue) override {
        llvm::LLVMContext& ctx = m.getContext();
        auto* i32 = llvm::Type::getInt32Ty(ctx);
        auto* fnTy = llvm::FunctionType::get(i32, {i32, i32}, /*vararg=*/false);
        llvm::FunctionCallee callee =
            m.getOrInsertFunction("__cajeta_xpu_cpu_spec_i32", fnTy);
        return b.CreateCall(
            callee,
            {llvm::ConstantInt::get(i32, slot),
             llvm::ConstantInt::get(i32, (uint64_t) (int64_t) defaultValue)},
            "spec.i32");
    }
    llvm::Value* specConstantF32(llvm::IRBuilderBase& b, llvm::Module& m,
                                 unsigned slot, float defaultValue) override {
        llvm::LLVMContext& ctx = m.getContext();
        auto* i32 = llvm::Type::getInt32Ty(ctx);
        auto* f32 = llvm::Type::getFloatTy(ctx);
        auto* fnTy = llvm::FunctionType::get(f32, {i32, f32}, /*vararg=*/false);
        llvm::FunctionCallee callee =
            m.getOrInsertFunction("__cajeta_xpu_cpu_spec_f32", fnTy);
        return b.CreateCall(callee,
                            {llvm::ConstantInt::get(i32, slot),
                             llvm::ConstantFP::get(f32, defaultValue)},
                            "spec.f32");
    }

    void decorateKernel(llvm::Function*, llvm::Module&) override {
        // Default C calling convention + external linkage (set at creation) is
        // exactly what the host driver / JIT looks up. Nothing to mark.
    }

    // Buffers as flat addrspace(0) pointers + scalars by value, then the 9
    // trailing i32 coordinate params. (Overrides the addrspace(1) default.)
    llvm::Function* createKernel(
        llvm::Module& m, const std::string& name,
        const std::vector<KernelParam>& params) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        std::vector<llvm::Type*> tys;
        tys.reserve(params.size() + kNumCoordParams);
        for (auto& p : params) {
            // Buffers, Texture2D, and Image2D handles are flat host pointers; a
            // Sampler rides by value as its {i32,i32} struct (p.type); scalars by
            // value. (An Image2D handle is the host image-record pointer — its
            // KernelParam.type is `float`, so without isImage here it would wrongly
            // arrive as a float scalar.)
            tys.push_back((p.isBuffer || p.isTexture || p.isImage)
                              ? (llvm::Type*) llvm::PointerType::get(ctx, 0)
                              : p.type);
        }
        for (unsigned i = 0; i < kNumCoordParams; ++i) tys.push_back(i32);

        auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), tys,
                                             /*vararg=*/false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage,
                                          name, &m);
        unsigned i = 0;
        for (auto& p : params) fn->getArg(i++)->setName(p.name);
        static const char* kCoordNames[kNumCoordParams] = {
            "tid.x", "tid.y", "tid.z", "ctaid.x", "ctaid.y",
            "ctaid.z", "ntid.x", "ntid.y", "ntid.z",
            "nctaid.x", "nctaid.y", "nctaid.z"};
        for (unsigned c = 0; c < kNumCoordParams; ++c)
            fn->getArg(i++)->setName(kCoordNames[c]);
        // Marks that this function carries the 12 trailing coord params, so
        // coord() can distinguish a kernel from a @Device helper (which the CPU
        // target lowers through the same builtin path but with no coord params).
        fn->addFnAttr("cajeta-cpu-kernel");
        decorateKernel(fn, m);
        return fn;
    }
    // materializeParam (fn->getArg) + bufferElementPtr (GEP) defaults are
    // correct: host pointers are addrspace 0, and the coordinate params live
    // past the materialized param indices, so they never collide.

    // A @Device helper's Buffer<T> param is a flat (addrspace 0) host pointer —
    // matching the buffer base createKernel hands the kernel (Item 2).
    llvm::Type* bufferParamType(llvm::Module& m, llvm::Type* /*elemTy*/) override {
        return llvm::PointerType::get(m.getContext(), 0);
    }

    // Texture sampling (Item 8): emit a call to the C runtime bilinear/nearest
    // sampler. `texHandle` is the host texobj pointer (the materialized texture
    // param); `samplerHandle` is the {i32 filterMode, i32 addressMode} struct
    // value (the materialized sampler param) — unpacked to two i32s here so the
    // runtime sees a flat signature. The math (clamp/wrap addressing + filtering)
    // lives in C, where it is easy to get right; the IR stays a single call.
    //
    //   float __cajeta_xpu_cpu_tex_sample(ptr tex, i32 filterMode,
    //                                     i32 addressMode, float u, float v)
    llvm::Value* sampleTexture(llvm::IRBuilderBase& b, llvm::Module& m,
                               llvm::Value* texHandle, llvm::Value* samplerHandle,
                               llvm::Value* u, llvm::Value* v,
                               llvm::Value* lod) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        llvm::Value* filterMode =
            b.CreateExtractValue(samplerHandle, {0}, "samp.filter");
        llvm::Value* addressMode =
            b.CreateExtractValue(samplerHandle, {1}, "samp.addr");
        // The C sampler returns the full RGBA texel as a <4 x float> and takes the
        // explicit mip `lod` (0.0 for the plain sample). Single-channel formats
        // expand G/B = 0, A = 1, so Texture2D.sample yields a Vector<float32,4>.
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        auto* fnTy = llvm::FunctionType::get(
            v4f, {llvm::PointerType::get(ctx, 0), i32, i32, f32, f32, f32},
            /*vararg=*/false);
        llvm::FunctionCallee callee =
            m.getOrInsertFunction("__cajeta_xpu_cpu_tex_sample_rgba", fnTy);
        if (auto* f = llvm::dyn_cast<llvm::Function>(callee.getCallee()))
            f->setDoesNotThrow();
        return b.CreateCall(callee,
                            {texHandle, filterMode, addressMode, u, v, lod},
                            "tex.sample");
    }

    // Texture2D.fetch(x, y) (texelFetch): the unfiltered, sampler-free exact
    // texel read. Emits a call to the C runtime `__cajeta_xpu_cpu_tex_fetch_rgba`,
    // which indexes the decoded float store directly (no addressing/filtering) —
    // the texel-read primitive sampleTexture's bilinear path is built on. No
    // Sampler arg (x, y are i32 texel indices).
    //
    //   <4 x float> __cajeta_xpu_cpu_tex_fetch_rgba(ptr tex, i32 x, i32 y)
    //   <4 x i32>   __cajeta_xpu_cpu_tex_fetch_rgba_i32(ptr tex, i32 x, i32 y)
    // The integer variant reinterprets the texel store as raw i32 (the upload
    // memcpy'd the integer bits in verbatim) — for Texture2D<int32>/<uint32>.
    llvm::Value* fetchTexture(llvm::IRBuilderBase& b, llvm::Module& m,
                              llvm::Value* texHandle, llvm::Value* x,
                              llvm::Value* y, llvm::Type* texelTy,
                              llvm::Value* lod) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        bool isInt = texelTy && texelTy->isIntegerTy();
        auto* v4t = llvm::FixedVectorType::get(
            isInt ? i32 : (llvm::Type*) llvm::Type::getFloatTy(ctx), 4);
        const char* sym = isInt ? "__cajeta_xpu_cpu_tex_fetch_rgba_i32"
                                : "__cajeta_xpu_cpu_tex_fetch_rgba";
        // The C fetch takes the explicit mip `lod` (0 for the plain fetch).
        auto* fnTy = llvm::FunctionType::get(
            v4t, {llvm::PointerType::get(ctx, 0), i32, i32, i32}, /*vararg=*/false);
        llvm::FunctionCallee callee = m.getOrInsertFunction(sym, fnTy);
        if (auto* f = llvm::dyn_cast<llvm::Function>(callee.getCallee()))
            f->setDoesNotThrow();
        return b.CreateCall(callee, {texHandle, x, y, lod}, "tex.fetch");
    }

    // Texture3D.sample(sampler, u, v, w): the 3-D trilinear sampler — calls the
    // C runtime __cajeta_xpu_cpu_tex3d_sample_rgba (a third coord vs the 2-D
    // sampler), returning the filtered <4 x float> voxel.
    llvm::Value* sampleTexture3D(llvm::IRBuilderBase& b, llvm::Module& m,
                                 llvm::Value* texHandle, llvm::Value* samplerHandle,
                                 llvm::Value* u, llvm::Value* v,
                                 llvm::Value* w) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        llvm::Value* filterMode =
            b.CreateExtractValue(samplerHandle, {0}, "samp.filter");
        llvm::Value* addressMode =
            b.CreateExtractValue(samplerHandle, {1}, "samp.addr");
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        auto* fnTy = llvm::FunctionType::get(
            v4f, {llvm::PointerType::get(ctx, 0), i32, i32, f32, f32, f32},
            /*vararg=*/false);
        llvm::FunctionCallee callee =
            m.getOrInsertFunction("__cajeta_xpu_cpu_tex3d_sample_rgba", fnTy);
        if (auto* f = llvm::dyn_cast<llvm::Function>(callee.getCallee()))
            f->setDoesNotThrow();
        return b.CreateCall(callee, {texHandle, filterMode, addressMode, u, v, w},
                            "tex3d.sample");
    }

    // Texture3D.fetch(x, y, z): the 3-D unfiltered voxel read — float or integer
    // variant by texel type, mirroring the 2-D fetch.
    //   <4 x float> __cajeta_xpu_cpu_tex3d_fetch_rgba(ptr, i32, i32, i32)
    //   <4 x i32>   __cajeta_xpu_cpu_tex3d_fetch_rgba_i32(ptr, i32, i32, i32)
    llvm::Value* fetchTexture3D(llvm::IRBuilderBase& b, llvm::Module& m,
                                llvm::Value* texHandle, llvm::Value* x,
                                llvm::Value* y, llvm::Value* z,
                                llvm::Type* texelTy) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        bool isInt = texelTy && texelTy->isIntegerTy();
        auto* v4t = llvm::FixedVectorType::get(
            isInt ? i32 : (llvm::Type*) llvm::Type::getFloatTy(ctx), 4);
        const char* sym = isInt ? "__cajeta_xpu_cpu_tex3d_fetch_rgba_i32"
                                : "__cajeta_xpu_cpu_tex3d_fetch_rgba";
        auto* fnTy = llvm::FunctionType::get(
            v4t, {llvm::PointerType::get(ctx, 0), i32, i32, i32}, /*vararg=*/false);
        llvm::FunctionCallee callee = m.getOrInsertFunction(sym, fnTy);
        if (auto* f = llvm::dyn_cast<llvm::Function>(callee.getCallee()))
            f->setDoesNotThrow();
        return b.CreateCall(callee, {texHandle, x, y, z}, "tex3d.fetch");
    }

    // Texture1D.sample(sampler, u): the 1-D linear sampler. On the CPU a 1-D
    // texture is just a 2-D texture with height = 1 (the alloc/upload runtime
    // builds it that way), so we reuse the 2-D sampler symbol with a constant
    // v = 0.5 (the center of the single row) and lod = 0 — no new C math. The
    // 2-D bilinear collapses to a 1-D lerp along u when there is one row.
    llvm::Value* sampleTexture1D(llvm::IRBuilderBase& b, llvm::Module& m,
                                 llvm::Value* texHandle, llvm::Value* samplerHandle,
                                 llvm::Value* u) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Value* v = llvm::ConstantFP::get(f32, 0.5);
        llvm::Value* lod = llvm::ConstantFP::get(f32, 0.0);
        return sampleTexture(b, m, texHandle, samplerHandle, u, v, lod);
    }

    // Texture1D.fetch(x): the 1-D unfiltered exact-texel read. Same height-1
    // reuse — call the 2-D fetch with y = 0, lod = 0.
    llvm::Value* fetchTexture1D(llvm::IRBuilderBase& b, llvm::Module& m,
                                llvm::Value* texHandle, llvm::Value* x,
                                llvm::Type* texelTy) override {
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        llvm::Value* y = llvm::ConstantInt::get(i32, 0);
        llvm::Value* lod = llvm::ConstantInt::get(i32, 0);
        return fetchTexture(b, m, texHandle, x, y, texelTy, lod);
    }

    // Texture2DArray.sample(sampler, u, v, layer): bilinear WITHIN the integer-
    // selected layer (no cross-layer filtering — unlike Texture3D's trilinear).
    // Emits a call to a dedicated C runtime symbol that addresses layer `layer`
    // (the array texobj stores layers exactly like a 3-D volume's z slices).
    //   <4 x float> __cajeta_xpu_cpu_tex2da_sample_rgba(ptr, i32 filt, i32 addr,
    //                                                   float u, float v, i32 layer)
    llvm::Value* sampleTexture2DArray(llvm::IRBuilderBase& b, llvm::Module& m,
                                      llvm::Value* texHandle,
                                      llvm::Value* samplerHandle, llvm::Value* u,
                                      llvm::Value* v, llvm::Value* layer) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        llvm::Value* filterMode =
            b.CreateExtractValue(samplerHandle, {0}, "samp.filter");
        llvm::Value* addressMode =
            b.CreateExtractValue(samplerHandle, {1}, "samp.addr");
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        auto* fnTy = llvm::FunctionType::get(
            v4f, {llvm::PointerType::get(ctx, 0), i32, i32, f32, f32, i32},
            /*vararg=*/false);
        llvm::FunctionCallee callee =
            m.getOrInsertFunction("__cajeta_xpu_cpu_tex2da_sample_rgba", fnTy);
        if (auto* f = llvm::dyn_cast<llvm::Function>(callee.getCallee()))
            f->setDoesNotThrow();
        return b.CreateCall(callee,
                            {texHandle, filterMode, addressMode, u, v, layer},
                            "tex2da.sample");
    }

    // Texture2DArray.fetch(x, y, layer): the exact-texel read of layer `layer`.
    // The array texobj stores layers as a 3-D volume's z slices, so this is
    // exactly the 3-D fetch with z = layer — reuse it (float or integer variant).
    llvm::Value* fetchTexture2DArray(llvm::IRBuilderBase& b, llvm::Module& m,
                                     llvm::Value* texHandle, llvm::Value* x,
                                     llvm::Value* y, llvm::Value* layer,
                                     llvm::Type* texelTy) override {
        return fetchTexture3D(b, m, texHandle, x, y, layer, texelTy);
    }

    // TextureCube.sample(sampler, x, y, z): project the direction onto a cube face
    // (major-axis selection, the standard +X,-X,+Y,-Y,+Z,-Z order) then bilinear
    // within that face. The 6 faces are stored like a 6-layer array (the cube
    // texobj's d = 6), so the C runtime does the projection + the per-face bilinear.
    //   <4 x float> __cajeta_xpu_cpu_texcube_sample_rgba(ptr, i32 filt, i32 addr,
    //                                                    float x, float y, float z)
    llvm::Value* sampleTextureCube(llvm::IRBuilderBase& b, llvm::Module& m,
                                   llvm::Value* texHandle, llvm::Value* samplerHandle,
                                   llvm::Value* x, llvm::Value* y,
                                   llvm::Value* z) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        llvm::Value* filterMode =
            b.CreateExtractValue(samplerHandle, {0}, "samp.filter");
        llvm::Value* addressMode =
            b.CreateExtractValue(samplerHandle, {1}, "samp.addr");
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        auto* fnTy = llvm::FunctionType::get(
            v4f, {llvm::PointerType::get(ctx, 0), i32, i32, f32, f32, f32},
            /*vararg=*/false);
        llvm::FunctionCallee callee =
            m.getOrInsertFunction("__cajeta_xpu_cpu_texcube_sample_rgba", fnTy);
        if (auto* f = llvm::dyn_cast<llvm::Function>(callee.getCallee()))
            f->setDoesNotThrow();
        return b.CreateCall(callee,
                            {texHandle, filterMode, addressMode, x, y, z},
                            "texcube.sample");
    }

    // Image2D storage images (the writable twin of Texture2D). On CPU the image
    // handle is a host pointer to the image record (a flat R32f float store), so
    // store/load lower to calls into the C runtime, like the texture fetch path —
    // no descriptor/surface object. `imgHandle` is the materialized param
    // (fn->getArg, a host pointer); `x`/`y` are i32 texel indices.
    //
    //   void  __cajeta_xpu_cpu_image_store(ptr img, i32 x, i32 y, float value)
    //   float __cajeta_xpu_cpu_image_load (ptr img, i32 x, i32 y)
    void storeImage(llvm::IRBuilderBase& b, llvm::Module& m,
                    llvm::Value* imgHandle, llvm::Value* x, llvm::Value* y,
                    llvm::Value* value) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        auto* fnTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx),
            {llvm::PointerType::get(ctx, 0), i32, i32, f32}, /*vararg=*/false);
        llvm::FunctionCallee callee =
            m.getOrInsertFunction("__cajeta_xpu_cpu_image_store", fnTy);
        if (auto* f = llvm::dyn_cast<llvm::Function>(callee.getCallee()))
            f->setDoesNotThrow();
        b.CreateCall(callee, {imgHandle, x, y, value});
    }

    llvm::Value* loadImage(llvm::IRBuilderBase& b, llvm::Module& m,
                           llvm::Value* imgHandle, llvm::Value* x,
                           llvm::Value* y) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        auto* fnTy = llvm::FunctionType::get(
            f32, {llvm::PointerType::get(ctx, 0), i32, i32}, /*vararg=*/false);
        llvm::FunctionCallee callee =
            m.getOrInsertFunction("__cajeta_xpu_cpu_image_load", fnTy);
        if (auto* f = llvm::dyn_cast<llvm::Function>(callee.getCallee()))
            f->setDoesNotThrow();
        return b.CreateCall(callee, {imgHandle, x, y}, "img.load");
    }

    // Wave ops. Each lowers to a *call* to its `__cajeta_xpu_wave_*` runtime
    // stub (width-1 scalar semantics: one work-item per host invocation). The
    // CPU registration pass then attaches a Vector Function ABI variant to each
    // stub so that when the per-block work-item loop is vectorized to the host's
    // native width W, LoopVectorize substitutes the SIMD `_vW` (or masked
    // `_Mv16` for divergent uses) variant — the wave becomes W SIMD lanes
    // (cajeta-cpu.md Inc 5C). If vectorization does not fire, the scalar call
    // runs — width-1, always correct. `width()` is the exception: it takes no
    // argument (a 0-arg VFABI variant is invalid), so registration rewrites it
    // to the constant W in a vectorized wave kernel.
    llvm::Value* waveWidth(llvm::IRBuilderBase& b, llvm::Module& m) override {
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        return pureCall(b, m, "__cajeta_xpu_wave_width", i32, {}, "wave.width");
    }
    llvm::Value* waveShuffle(llvm::IRBuilderBase& b, llvm::Module& m,
                             llvm::Value* value, llvm::Value* srcLane) override {
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        return pureCall(b, m, "__cajeta_xpu_wave_shuffle_sync_u32", i32,
                        {value, srcLane}, "wave.shuffle");
    }
    llvm::Value* waveBallot(llvm::IRBuilderBase& b, llvm::Module& m,
                            llvm::Value* pred) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i1 = llvm::Type::getInt1Ty(ctx);
        if (!pred->getType()->isIntegerTy(1))      // normalize boolean → i1
            pred = b.CreateICmpNE(pred,
                                  llvm::ConstantInt::get(pred->getType(), 0));
        return pureCall(b, m, "__cajeta_xpu_wave_ballot_sync",
                        llvm::Type::getInt64Ty(ctx), {pred}, "wave.ballot");
    }
    llvm::Value* waveReduceSum(llvm::IRBuilderBase& b, llvm::Module& m,
                               llvm::Value* value) override {
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        return pureCall(b, m, "__cajeta_xpu_wave_reduce_sum_u32", i32, {value},
                        "wave.reducesum");
    }
    llvm::Value* waveReduce(llvm::IRBuilderBase& b, llvm::Module& m,
                            WaveReduceOp op, llvm::Value* value) override {
        // Mirror reduceSum: a pure runtime wave stub whose VFABI vector variant
        // (CpuRegistration) does the cross-lane reduce when the kernel widens.
        const char* sym;
        switch (op) {
            case WaveReduceOp::Max: sym = "__cajeta_xpu_wave_reduce_max_u32"; break;
            case WaveReduceOp::Min: sym = "__cajeta_xpu_wave_reduce_min_u32"; break;
            case WaveReduceOp::And: sym = "__cajeta_xpu_wave_reduce_and_u32"; break;
            case WaveReduceOp::Or:  sym = "__cajeta_xpu_wave_reduce_or_u32"; break;
            case WaveReduceOp::Xor: sym = "__cajeta_xpu_wave_reduce_xor_u32"; break;
        }
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        return pureCall(b, m, sym, i32, {value}, "wave.reduce");
    }
    llvm::Value* waveReduceF32(llvm::IRBuilderBase& b, llvm::Module& m,
                               WaveReduceFOp op, llvm::Value* value) override {
        // Mirror the integer family: a pure f32 runtime stub whose VFABI
        // vector variant (CpuRegistration) does the cross-lane float reduce
        // when the kernel widens (10.12.38).
        const char* sym = op == WaveReduceFOp::Sum
            ? "__cajeta_xpu_wave_reduce_sum_f32"
            : "__cajeta_xpu_wave_reduce_max_f32";
        llvm::Type* f32 = llvm::Type::getFloatTy(m.getContext());
        return pureCall(b, m, sym, f32, {value}, "wave.reducef");
    }
    llvm::Value* waveScan(llvm::IRBuilderBase& b, llvm::Module& m,
                          WaveScanOp op, llvm::Value* value) override {
        // A pure runtime stub whose VFABI vector variant does the in-lane
        // exclusive prefix scan when the kernel widens (the base Hillis-Steele
        // default's shuffle loop doesn't vectorize on the CPU wave model).
        const char* sym = op == WaveScanOp::Sum
            ? "__cajeta_xpu_wave_prefix_sum_u32"
            : "__cajeta_xpu_wave_prefix_product_u32";
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        return pureCall(b, m, sym, i32, {value}, "wave.scan");
    }
    // Lane within the wave = the block-local work-item index modulo the wave
    // width. width() is rewritten to the constant W in a vectorized wave kernel,
    // so this folds to `tid.x % W`; vectorized, the W-aligned vector induction
    // yields lanes 0..W-1. In a width-1 fallback width() → 1, so laneId → 0.
    llvm::Value* waveLaneId(llvm::IRBuilderBase& b, llvm::Module& m) override {
        return b.CreateURem(threadId(b, m, 0), waveWidth(b, m), "wave.laneid");
    }

private:
    // Emit a call to a pure (memory-none, willreturn, nounwind) runtime wave
    // stub — the marking LoopVectorize needs to be willing to widen the call
    // into its VFABI variant.
    static llvm::Value* pureCall(llvm::IRBuilderBase& b, llvm::Module& m,
                                 const char* name, llvm::Type* retTy,
                                 llvm::ArrayRef<llvm::Value*> args,
                                 const char* twine) {
        std::vector<llvm::Type*> argTys;
        argTys.reserve(args.size());
        for (auto* a : args) argTys.push_back(a->getType());
        auto* fnTy = llvm::FunctionType::get(retTy, argTys, /*vararg=*/false);
        llvm::FunctionCallee callee = m.getOrInsertFunction(name, fnTy);
        if (auto* f = llvm::dyn_cast<llvm::Function>(callee.getCallee())) {
            f->setDoesNotThrow();
            f->setWillReturn();
            f->setMemoryEffects(llvm::MemoryEffects::none());
        }
        auto* call = b.CreateCall(callee, args, twine);
        call->setDoesNotThrow();
        return call;
    }

    // Read coordinate (group + dim) from the 12 trailing kernel args, laid out
    // [tid.xyz, ctaid.xyz, ntid.xyz, nctaid.xyz]. group ∈ {0,3,6,9}, dim ∈ {0,1,2}.
    static llvm::Value* coord(llvm::IRBuilderBase& b, unsigned group,
                              unsigned dim) {
        llvm::Function* fn = b.GetInsertBlock()->getParent();
        // Coords live only on functions createKernel built (the last 12 args).
        // A @Device helper reaching a thread/workgroup builtin on the CPU
        // backend has none — without this guard arg_size()-12 underflows and
        // getArg() reads out of bounds. The other backends read hardware
        // intrinsics, so this restriction is CPU-only.
        if (!fn->hasFnAttribute("cajeta-cpu-kernel") ||
            fn->arg_size() < kNumCoordParams)
            throw cajeta::Exception(
                "XPU CPU backend: thread/workgroup coordinate builtins are not "
                "supported inside a @Device helper (only in the @Kernel body); "
                "pass the coordinate in as a parameter instead",
                "XPU-C01");
        unsigned n = fn->arg_size();
        return fn->getArg(n - kNumCoordParams + group + dim);
    }
};

} // namespace

llvm::Function* lowerKernel(const MethodPtr& method,
                            llvm::Module& deviceModule) {
    CpuTarget target;
    return cajeta::xpu::lowerKernel(method, deviceModule, target);
}

} // namespace cpu
} // namespace xpu
} // namespace cajeta
