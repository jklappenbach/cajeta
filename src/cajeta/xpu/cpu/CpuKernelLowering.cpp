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

// The CPU LoweringTarget. The host has no coordinate intrinsics, so a work-item's
// coordinates arrive as the 12 trailing i32 kernel args the coord reads pull back.
class CpuTarget : public LoweringTarget {
public:
    const char* name() const override { return "cpu"; }

    // Wide `dotAccum` for the host ISA, so a @Kernel reaches the tier an ordinary
    // method does. Null (leaving the portable reduce) off x86, without VNNI, or on
    // any shape but 4·n int8 lanes into n i32 accumulators.
    llvm::Value* integerDotWide(llvm::IRBuilderBase& b, llvm::Module& m,
                                llvm::Value* w, llvm::Value* a,
                                llvm::Value* acc, bool wUnsigned) override {
        const char* fsd = std::getenv("CAJETA_SIMD_SCALAR_FALLBACK");
        if (fsd && fsd[0] && fsd[0] != '0') return nullptr;
        static const std::unique_ptr<llvm::TargetMachine> tm =
            createCpuTargetMachine();
        if (!tm || !tm->getTargetTriple().isX86()) return nullptr;
        const llvm::MCSubtargetInfo& sti = tm->getMCSubtargetInfo();
        if (!sti.checkFeatures("+avx512vnni") && !sti.checkFeatures("+avxvnni"))
            return nullptr;

        // VNNI only, shape checked here rather than left to dotAccum's tier order:
        // its AVX2 tier saturates, and this backend is the GPU backends' bit-exact
        // oracle. `vpdpbusd` never saturates.
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

    // No native inline ray query: the noun is a software BVH, so RayQuery walks it.
    NounImpl accelImpl() const override { return NounImpl::SoftwareBvh; }

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

    // Grid-stride stride: work-items in `dim` as i32, nctaid·ntid, from coord params.
    llvm::Value* gridSize(llvm::IRBuilderBase& b, llvm::Module&,
                          unsigned dim) override {
        return b.CreateMul(coord(b, /*group=*/9, dim),   // nctaid.{x,y,z}
                           coord(b, /*group=*/6, dim),   // ntid.{x,y,z}
                           "xpu.gridsize");
    }

    // Emits the marker call the registration pass fissions the work-item loop at. It
    // stays impure, noinline and noduplicate so nothing deletes or clones it first.
    void workgroupBarrier(llvm::IRBuilderBase& b, llvm::Module& m) override {
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

    // Calls libc `printf` (a CPU kernel is host code under LLJIT), f32 args promoted
    // to double as C varargs require.
    void devicePrintf(llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* fmt,
                      llvm::ArrayRef<llvm::Value*> args) override {
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

    // Reads the specialization constant at runtime rather than baking the literal, so
    // a host override needs no recompile; the helper returns `defaultValue` if none.
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

    // Nothing to mark: the C calling convention and external linkage set at creation
    // are exactly what the host driver and JIT look up.
    void decorateKernel(llvm::Function*, llvm::Module&) override {
    }

    // Buffer, texture and image handles as flat addrspace(0) pointers (overriding the
    // addrspace(1) default) and scalars by value, then the i32 coordinate params.
    llvm::Function* createKernel(
        llvm::Module& m, const std::string& name,
        const std::vector<KernelParam>& params) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        std::vector<llvm::Type*> tys;
        tys.reserve(params.size() + kNumCoordParams);
        for (auto& p : params) {
            // An Image2D handle is a host image-record pointer, but its
            // KernelParam.type is `float`: without isImage it arrives as a scalar.
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
        // How coord() tells a kernel from a @Device helper, which has no coord params.
        fn->addFnAttr("cajeta-cpu-kernel");
        decorateKernel(fn, m);
        return fn;
    }

    // A @Device helper's Buffer<T> param is the flat host pointer kernels also get.
    llvm::Type* bufferParamType(llvm::Module& m, llvm::Type* /*elemTy*/) override {
        return llvm::PointerType::get(m.getContext(), 0);
    }

    // Bilinear/nearest sample through the C runtime, which owns the addressing math.
    // `samplerHandle`'s {filterMode, addressMode} struct is unpacked to two i32s.
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

    // Unfiltered exact-texel read at i32 indices: the runtime indexes the decoded
    // store directly, or, for an integer texel type, reinterprets it as raw bits.
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
        auto* fnTy = llvm::FunctionType::get(
            v4t, {llvm::PointerType::get(ctx, 0), i32, i32, i32}, /*vararg=*/false);
        llvm::FunctionCallee callee = m.getOrInsertFunction(sym, fnTy);
        if (auto* f = llvm::dyn_cast<llvm::Function>(callee.getCallee()))
            f->setDoesNotThrow();
        return b.CreateCall(callee, {texHandle, x, y, lod}, "tex.fetch");
    }

    // Trilinear 3-D sample: the 2-D sampler's runtime call with a third coordinate.
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

    // Unfiltered 3-D voxel read, float or integer variant by texel type.
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

    // A 1-D texture is allocated height-1, so reuse the 2-D sampler at v = 0.5.
    llvm::Value* sampleTexture1D(llvm::IRBuilderBase& b, llvm::Module& m,
                                 llvm::Value* texHandle, llvm::Value* samplerHandle,
                                 llvm::Value* u) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Value* v = llvm::ConstantFP::get(f32, 0.5);
        llvm::Value* lod = llvm::ConstantFP::get(f32, 0.0);
        return sampleTexture(b, m, texHandle, samplerHandle, u, v, lod);
    }

    // The 1-D exact-texel read: the same height-1 reuse, with y = 0.
    llvm::Value* fetchTexture1D(llvm::IRBuilderBase& b, llvm::Module& m,
                                llvm::Value* texHandle, llvm::Value* x,
                                llvm::Type* texelTy) override {
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        llvm::Value* y = llvm::ConstantInt::get(i32, 0);
        llvm::Value* lod = llvm::ConstantInt::get(i32, 0);
        return fetchTexture(b, m, texHandle, x, y, texelTy, lod);
    }

    // Bilinear within the selected layer only, so not the trilinear 3-D symbol.
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

    // Layers are stored as a volume's z slices, so this is the 3-D fetch at z = layer.
    llvm::Value* fetchTexture2DArray(llvm::IRBuilderBase& b, llvm::Module& m,
                                     llvm::Value* texHandle, llvm::Value* x,
                                     llvm::Value* y, llvm::Value* layer,
                                     llvm::Type* texelTy) override {
        return fetchTexture3D(b, m, texHandle, x, y, layer, texelTy);
    }

    // The runtime projects the direction onto a cube face, then filters within it.
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

    // Image2D store and load: `imgHandle` points at a flat R32f host image record.
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

    // Wave ops call `__cajeta_xpu_wave_*` stubs with width-1 scalar semantics;
    // registration attaches VFABI variants so LoopVectorize widens them to the host
    // width W. `width()` takes no argument, so registration rewrites it to W instead.
    llvm::Value* waveWidth(llvm::IRBuilderBase& b, llvm::Module& m) override {
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        return pureCall(b, m, "__cajeta_xpu_wave_width", i32, {}, "wave.width");
    }
    // The cooperative unit is one work-item, and a literal keeps registration from
    // widening this or flagging a wave kernel.
    llvm::Value* groupWidth(llvm::IRBuilderBase& b, llvm::Module& m) override {
        (void) b;
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(m.getContext()), 1);
    }
    // The single lane of a width-1 group is lane 0.
    llvm::Value* groupLaneId(llvm::IRBuilderBase& b, llvm::Module& m) override {
        (void) b;
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(m.getContext()), 0);
    }
    // Identity, not the wave reduce: the SIMD lanes each run a different group, so a
    // cross-lane sum would merge independent rows the single group lane already has.
    llvm::Value* groupReduceF32(llvm::IRBuilderBase& b, llvm::Module& m,
                                WaveReduceFOp op, llvm::Value* value) override {
        (void) b; (void) m; (void) op;
        return value;
    }
    llvm::Value* groupReduceF32Segmented(llvm::IRBuilderBase& b, llvm::Module& m,
                                         WaveReduceFOp op, llvm::Value* value,
                                         llvm::Value* seg) override {
        (void) b; (void) m; (void) op; (void) seg;
        return value;
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
        if (!pred->getType()->isIntegerTy(1))
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
        const char* sym = op == WaveReduceFOp::Sum
            ? "__cajeta_xpu_wave_reduce_sum_f32"
            : "__cajeta_xpu_wave_reduce_max_f32";
        llvm::Type* f32 = llvm::Type::getFloatTy(m.getContext());
        return pureCall(b, m, sym, f32, {value}, "wave.reducef");
    }

    // Routes to the whole-wave reduce, which vectorizes where the base butterfly does
    // not: a segment (a quant block of 32 or 256) always covers the <= 16-lane wave.
    llvm::Value* waveReduceF32Segmented(llvm::IRBuilderBase& b, llvm::Module& m,
                                        WaveReduceFOp op, llvm::Value* value,
                                        llvm::Value* /*segment*/) override {
        return waveReduceF32(b, m, op, value);
    }
    // A stub, not the base Hillis-Steele shuffle loop, which does not vectorize here.
    llvm::Value* waveScan(llvm::IRBuilderBase& b, llvm::Module& m,
                          WaveScanOp op, llvm::Value* value) override {
        const char* sym = op == WaveScanOp::Sum
            ? "__cajeta_xpu_wave_prefix_sum_u32"
            : "__cajeta_xpu_wave_prefix_product_u32";
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        return pureCall(b, m, sym, i32, {value}, "wave.scan");
    }
    // The work-item index modulo the wave width; lane 0 in the width-1 fallback.
    llvm::Value* waveLaneId(llvm::IRBuilderBase& b, llvm::Module& m) override {
        return b.CreateURem(threadId(b, m, 0), waveWidth(b, m), "wave.laneid");
    }

private:
    // Calls a wave stub marked memory-none/willreturn/nounwind, as widening requires.
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
        // A @Device helper has no coord params: unguarded, the index below underflows.
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
