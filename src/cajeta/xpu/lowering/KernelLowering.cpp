//
// Shared @Kernel AST → device llvm::Function lowering — see header.
//

#include "KernelLowering.h"
#include "LoweringTarget.h"

#include "../../method/Method.h"
#include "../../type/FormalParameter.h"
#include "../../type/CajetaType.h"
#include "../../type/CajetaClass.h"
#include "../../type/CajetaArray.h"
#include "../../type/CajetaVector.h"
#include "../../type/CajetaMatrix.h"
#include "../../type/CajetaQuaternion.h"
#include "../../type/CajetaConstantType.h"
#include "../../type/VectorOps.h"
#include "../../type/MatrixOps.h"
#include "../../type/QuaternionOps.h"
#include "../core/XpuAttributes.h"
#include "../core/KernelArgTrait.h"
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
#include "../../asn/expression/OperatorDispatch.h"
#include "../../asn/expression/LiteralExpression.h"
#include "../../asn/expression/NewExpression.h"
#include "../../asn/expression/CreatorRest.h"
#include "../../asn/expression/DotExpression.h"
#include "../../type/StructureProperty.h"

#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace cajeta {
namespace xpu {

// Global thread index = workgroupId * workgroupDim + threadId. This identity
// holds on both NVPTX (ctaid*ntid+tid) and AMDGPU (workgroup.id*wgsize+
// workitem.id), so it lives in the shared base; a backend with a native
// global-id intrinsic can override.
llvm::Value* LoweringTarget::globalId(llvm::IRBuilderBase& b, llvm::Module& m,
                                      unsigned dim) {
    llvm::Value* wid = workgroupId(b, m, dim);
    llvm::Value* wdim = workgroupDim(b, m, dim);
    llvm::Value* tid = threadId(b, m, dim);
    return b.CreateAdd(b.CreateMul(wid, wdim), tid, "gid");
}

// The explicit-override layer of the degrade seam (CajetaGPU.md §1.5, inc-4
// brick #4): apply CAJETA_GPU_<FEATURE>_IMPL to a per-backend base tier. The
// override wins when set ("software" → Portable, "native" → Native); unset or
// any other value keeps `base`. Read here and ONLY here — the compile-time-
// feature instance of the CAJETA_GPU_<FEATURE>_IMPL convention (the runtime-noun
// instance is caj_resolve_as_impl / CAJETA_GPU_AS_IMPL in cajeta_runtime.c).
// Precedence + the case-sensitive string match mirror caj_resolve_as_impl.
LoweringTarget::ImplTier resolveImplTier(const char* feature,
                                         LoweringTarget::ImplTier base) {
    std::string var = std::string("CAJETA_GPU_") + feature + "_IMPL";
    const char* env = std::getenv(var.c_str());
    if (env && *env) {
        if (std::string(env) == "software") return LoweringTarget::ImplTier::Portable;
        if (std::string(env) == "native")   return LoweringTarget::ImplTier::Native;
        // unknown value: ignore; keep the base decision.
    }
    return base;
}

// Defined far below (after the anonymous-namespace block); forward-declared in
// cajeta::xpu so DeviceLowerer (in that block) can lower @Device helpers.
static std::vector<LoweringTarget::KernelParam> collectParams(
        const MethodPtr& method, llvm::LLVMContext& ctx);

namespace {

[[noreturn]] void unsupported(const std::string& what) {
    throw cajeta::Exception(
        "XPU kernel lowering: unsupported construct — " + what,
        "XPU-N01");
}

// addrspace(1) == device global memory, addrspace(3) == workgroup / shared
// memory. NVPTX and AMDGPU agree on both (AddressSpace.h: Global->1,
// Shared->3), so these are shared constants, not a fork point.
constexpr unsigned kGlobalAS = 1;
constexpr unsigned kSharedAS = 3;

// Map a primitive CajetaType to a device LLVM scalar type, built fresh
// in the device context (NOT via CajetaType::getLlvmType(), whose cache
// is bound to the host context). Returns nullptr for non-primitives.
llvm::Type* deviceScalarType(const CajetaTypePtr& t, llvm::LLVMContext& ctx) {
    if (!t) return nullptr;
    unsigned long f = t->getTypeFlags();
    if (!(f & PRIMITIVE_FLAG)) return nullptr;
    if (f & FLOAT_FLAG) {
        if (f & BIT_64_FLAG) return llvm::Type::getDoubleTy(ctx);
        if (f & BIT_16_FLAG) {
            // float16 (binary16) -> half; bfloat16 -> bfloat. Distinguished by
            // the type-ID byte (both are FLOAT|BIT_16).
            constexpr unsigned long kIdMask = 0x000000FF00000000UL;
            return (f & kIdMask) == BFLOAT16_ID
                ? llvm::Type::getBFloatTy(ctx) : llvm::Type::getHalfTy(ctx);
        }
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

// Map a Vector<T,N> CajetaType to a device LLVM `<N x T>`, built fresh in the
// device context. Returns nullptr when `t` is not a CajetaVector (or its
// element type isn't a device scalar). The element-type/lane data is read
// structurally off the CajetaVector — the device walker carries no resolved
// types, but a Vector local's declared type is a CajetaVector regardless.
llvm::Type* deviceVectorType(const CajetaTypePtr& t, llvm::LLVMContext& ctx) {
    auto vec = std::dynamic_pointer_cast<CajetaVector>(t);
    if (!vec) return nullptr;
    llvm::Type* elem = deviceScalarType(vec->getElementType(), ctx);
    if (!elem) return nullptr;
    // L3: reject a zero-lane vector cleanly — FixedVectorType::get(elem, 0)
    // asserts/aborts in a debug LLVM and yields degenerate IR otherwise.
    if (vec->getLanes() == 0)
        throw cajeta::Exception(
            "XPU kernel lowering: Vector<T, 0> has no lanes — N must be > 0",
            "XPU-N01");
    return llvm::FixedVectorType::get(elem, vec->getLanes());
}

// Map a Matrix<T,R,C> CajetaType to a device LLVM `<R*C x T>` (flat row-major),
// built fresh in the device context. Returns nullptr when `t` isn't a
// CajetaMatrix. Same flat representation the host uses (B1); the device walker
// tracks (R,C) by name since the LLVM type alone can't tell a Matrix<2,3> from
// a Vector<6>.
llvm::Type* deviceMatrixType(const CajetaTypePtr& t, llvm::LLVMContext& ctx) {
    auto mat = std::dynamic_pointer_cast<CajetaMatrix>(t);
    if (!mat) return nullptr;
    llvm::Type* elem = deviceScalarType(mat->getElementType(), ctx);
    if (!elem) return nullptr;
    if (mat->getRows() == 0 || mat->getCols() == 0)
        throw cajeta::Exception(
            "XPU kernel lowering: Matrix<T,0,..> / <..,0> has no lanes",
            "XPU-N01");
    return llvm::FixedVectorType::get(elem, mat->getRows() * mat->getCols());
}

// Map a Quaternion<T> CajetaType to a device LLVM `<4 x T>` (w, x, y, z), or
// nullptr when `t` isn't a CajetaQuaternion. The device walker tracks
// quaternion-ness by name (a Quaternion shares the `<4 x T>` slot with a
// Vector<T,4>) so `*` = Hamilton product / rotation and the methods are routed.
llvm::Type* deviceQuaternionType(const CajetaTypePtr& t, llvm::LLVMContext& ctx) {
    auto q = std::dynamic_pointer_cast<CajetaQuaternion>(t);
    if (!q) return nullptr;
    llvm::Type* elem = deviceScalarType(q->getElementType(), ctx);
    if (!elem) return nullptr;
    return llvm::FixedVectorType::get(elem, 4);
}

bool isBufferType(const CajetaTypePtr& t) {
    if (!t) return false;
    const std::string& c = t->toCanonical();
    static const std::string kPrefix = "cajeta.gpu.core.Buffer";
    return c.compare(0, kPrefix.size(), kPrefix) == 0;
}

bool typeIsSigned(const CajetaTypePtr& t) {
    return t && (t->getTypeFlags() & SIGNED_FLAG);
}

// A POD struct kernel param, lowered to a device LLVM struct of its primitive
// fields in declaration order. The host class carries a vtable pointer at LLVM
// slot 0; that word is STRIPPED here (and by the launch-site marshaller in
// CallExpression.cpp) — a host pointer is meaningless on the device and a
// pointer inside a struct is invalid in the SPIR-V storage-buffer model. So the
// device struct is { field0, field1, ... } and field i lives at index i.
// Built as a literal StructType (uniqued by body), so the type collectParams
// puts in the signature and the one lowerBody rebuilds for field GEPs are
// identical. `type` is null when `t` is not a POD struct.
struct DeviceStructInfo {
    llvm::StructType* type = nullptr;
    // `sub` is non-empty only for a nested @ValueType field (S5): it carries the
    // field's own field map so a two-level read `param.vfield.subfield` resolves
    // to a multi-index extractvalue. libstdc++ std::map supports the incomplete
    // value type here (C++17).
    struct Field {
        unsigned index;
        llvm::Type* type;
        bool isSigned;
        std::map<std::string, Field> sub;
    };
    std::map<std::string, Field> fields;
};

DeviceStructInfo deviceStructInfo(const CajetaTypePtr& t, llvm::LLVMContext& ctx) {
    DeviceStructInfo info;
    auto cls = std::dynamic_pointer_cast<CajetaClass>(t);
    if (!cls) return info;
    if (cls->isInterface() || isBufferType(t)) return info;
    if (cls->countInheritedFields() != 0) return info;   // no inheritance v1
    std::vector<llvm::Type*> ftys;
    unsigned idx = 0;
    for (auto& prop : cls->getPropertyList()) {
        if (!prop || prop->isStatic()) continue;
        CajetaTypePtr pt = prop->getType();
        llvm::Type* fty = deviceScalarType(pt, ctx);
        if (fty) {
            info.fields[prop->getName()] = {idx, fty, typeIsSigned(pt), {}};
            ftys.push_back(fty);
            ++idx;
            continue;
        }
        // S5: a nested @ValueType field is itself a flat by-value POD — recurse
        // into a nested device struct and remember its field map for two-level
        // reads. A value-type-containing struct stays POD all the way down.
        if (pt && (pt->getTypeFlags() & VALUE_TYPE_FLAG)) {
            DeviceStructInfo nested = deviceStructInfo(pt, ctx);
            if (nested.type) {
                DeviceStructInfo::Field f{idx, nested.type, false, {}};
                f.sub = nested.fields;
                info.fields[prop->getName()] = std::move(f);
                ftys.push_back(nested.type);
                ++idx;
                continue;
            }
        }
        return DeviceStructInfo{};                        // non-POD field
    }
    if (ftys.empty()) return DeviceStructInfo{};
    info.type = llvm::StructType::get(ctx, ftys);
    return info;
}

// Number of coordinate components a texture kind's sample/fetch takes, by the
// KernelParam::textureDim kind code (1=1D, 2=2D, 3=3D, 4=2D-array, 5=cube). The
// linear kinds (1/2/3) have arity = the dim; a 2-D array is (u,v,layer) and a
// cube is (x,y,z) direction — both 3. (Distinct from the SPIR-V image Dim.)
static inline int textureCoordArity(int dim) {
    return dim <= 3 ? dim : 3;
}

// One device kernel's worth of lowering state.
class DeviceLowerer {
public:
    using DeviceFnCache = std::map<const Method*, llvm::Function*>;

    DeviceLowerer(llvm::Module& m, llvm::Function* fn, LoweringTarget& target)
        : mod(m), ctx(m.getContext()), builder(ctx), fn(fn), target(target) {}

    // @Device helper-call context: `cls` resolves a bare helper name to its
    // sibling method; `deviceFns` is a cache of already-lowered @Device functions
    // shared across the kernel and all helpers (a nullptr entry = currently being
    // lowered → a recursive call, which is rejected).
    void setDeviceContext(std::shared_ptr<CajetaClass> c, DeviceFnCache* cache) {
        cls = std::move(c);
        deviceFns = cache;
    }

    // The admitted kernel params (computed by lowerKernel); materialized into
    // the entry block by lowerBody via target.materializeParam (allocas /
    // descriptor binds need the entry block to exist first).
    void setParams(std::vector<LoweringTarget::KernelParam> p) {
        kparams = std::move(p);
    }

    // True when lowering a @Device helper: its params are ordinary LLVM
    // function arguments (the caller passes already-materialized values), so
    // lowerBody reads fn->getArg(idx) directly instead of target.materializeParam
    // — which on Vulkan would (wrongly) bind a fresh descriptor/SSBO per param.
    void setParamsAsArgs(bool b) { paramsAsArgs = b; }

    // True iff the lowered body used a cross-lane subgroup op (shuffle/ballot/
    // reduce). Drives the maximal-reconvergence request at finalization.
    bool usedSubgroupOp() const { return usedSubgroupOp_; }

    void lowerBody(const MethodPtr& method) {
        if (!cls) cls = method->getParent();   // for @Device helper resolution
        builder.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", fn));
        // @FastMath: relax IEEE FP for every op the body emits — the backend may
        // contract to FMA, reassociate, use reciprocals, and pick approximate
        // transcendentals. Set on the IRBuilder so all subsequently-created FP
        // instructions (and the transcendental seam's intrinsic calls) carry it.
        if (method && isFastMath(*method)) {
            llvm::FastMathFlags fmf;
            fmf.setFast();   // contract + reassoc + arcp + afn + nnan/ninf/nsz
            builder.setFastMathFlags(fmf);
        }
        // Materialize params now that the entry block exists. Scalars get a
        // mutable alloca slot (so they can be reassigned / serve as loop
        // counters); buffers keep their base/handle. HOW a param's runtime
        // value is obtained is the backend's call (target.materializeParam):
        // NVPTX/AMDGPU read fn->getArg(idx); Vulkan binds a descriptor here.
        unsigned idx = 0;
        for (auto& p : kparams) {
            // Bindless buffer array (Buffer<T>[]): NO single descriptor to bind —
            // `bufs[idx]` selects one per access (bufferArrayElement). Record the
            // binding (= param index) + element type; on CPU the materialized arg
            // is the [count, h…] handle-array base pointer.
            if (p.isBufferArray) {
                bufferArrayBindings[p.name] = {idx, p.type, p.isSigned};
                // Pointer backends (CPU/NVPTX/AMD) take the marshalled [count, h…]
                // handle array as the param's value (materializeParam → fn->getArg
                // for the kernel, or the plain helper arg); Vulkan binds
                // per-access via handlefrombinding (no prologue value), so base
                // stays null. NOTE: `paramsAsArgs` is the @Device-helper flag, NOT
                // the kernel/Vulkan distinction — use descriptorBoundParams().
                bufferArrayBases[p.name] =
                    target.descriptorBoundParams()
                        ? nullptr
                        : (paramsAsArgs
                               ? fn->getArg(idx)
                               : target.materializeParam(builder, mod, fn, idx, p));
                ++idx;
                continue;
            }
            // Kernel params go through the backend (descriptor binds on Vulkan);
            // helper params are plain fn args, taken directly (setParamsAsArgs).
            llvm::Value* v = paramsAsArgs
                ? fn->getArg(idx)
                : target.materializeParam(builder, mod, fn, idx, p);
            ++idx;
            if (p.isAccelStruct) {
                // AccelerationStructure handle (Part C): the materialized
                // descriptor; read only by `rq.initialize(as, ...)`.
                accelHandles[p.name] = v;
            } else if (p.isTexture) {
                // Texture2D handle (Item 8): kept as the materialized backend
                // value (a ptr on CPU); read by `tex.sample(...)`/`tex.fetch(...)`.
                // Also remember the texel scalar T so fetch builds the right
                // result vector and sample can reject an integer texture.
                textureHandles[p.name] = v;
                textureTexelTypes[p.name] = p.type;
                textureDims[p.name] = p.textureDim;
            } else if (p.isImage) {
                // Image2D handle (writable images): the materialized STORAGE_IMAGE
                // descriptor; written only by `img.store(x, y, v)`.
                imageHandles[p.name] = v;
            } else if (p.isSampler) {
                // Sampler descriptor (Item 8): the materialized {i32,i32} value;
                // consumed by `tex.sample(sampler, ...)` via the backend seam.
                samplerHandles[p.name] = v;
            } else if (p.isBuffer) {
                bufferBases[p.name] = v;
                bufferElems[p.name] = p.type;
                bufferElemSigned[p.name] = p.isSigned;
            } else if (p.type->isStructTy()) {
                // Read-only POD struct param (Item 7): keep the materialized
                // aggregate as an SSA value and read fields via extractvalue —
                // NO alloca round-trip, so it stays valid under SPIR-V logical
                // addressing (an aggregate store to a Function-storage pointer
                // is rejected by spirv-val: "not a logical pointer").
                structValues[p.name] = v;
            } else {
                llvm::Value* slot = entryAlloca(p.type, p.name);
                builder.CreateStore(v, slot);
                values[p.name] = slot;
                slotTypes[p.name] = p.type;
                signedness[p.name] = p.isSigned;
            }
        }
        // Field maps for POD-struct params, so `name.field` reads resolve to a
        // GEP into the param's alloca slot (kept in `values`).
        for (auto& p : method->getParameterList()) {
            if (!p || p->getName() == "this") continue;
            DeviceStructInfo si = deviceStructInfo(p->getType(), ctx);
            if (si.type) structFields[p->getName()] = std::move(si);
            // Matrix<T,R,C> param (B1 follow-on): the materialize loop gave it a
            // flat <R*C x T> slot via the else branch; record its (R,C) shape so
            // m[r][c], `*`=matmul, and the methods recognize it as a matrix
            // (a Matrix<2,3> and a Vector<6> share the <6 x T> slot type).
            if (auto mt = std::dynamic_pointer_cast<CajetaMatrix>(p->getType()))
                matrixShapes[p->getName()] = {mt->getRows(), mt->getCols()};
            // S8: record value-type-typed param names so `a OP b` can recover
            // the operand's class for @Device operator resolution.
            if (p->getType() && p->getType()->isValueType())
                valueTypeNames[p->getName()] = p->getType();
        }
        // S8: value-type classes this body can construct (`new Vec2(...)`),
        // keyed by simple name — the operand value types plus the declaring
        // class itself (a value-type-returning @Device operator builds its own
        // type by value). Registered for the construction interception below.
        auto registerCtor = [&](const std::shared_ptr<CajetaClass>& c) {
            if (c && c->isValueType())
                valueTypeCtors[c->getQName()->getTypeName()] = c;
        };
        for (auto& [n, t] : valueTypeNames)
            registerCtor(std::dynamic_pointer_cast<CajetaClass>(t));
        registerCtor(method->getParent());
        lowerStatement(method->getBlock());
        // Kernels return void; close any open block.
        if (!builder.GetInsertBlock()->hasTerminator()) {
            builder.CreateRetVoid();
        }
    }

private:
    llvm::Module& mod;
    llvm::LLVMContext& ctx;
    llvm::IRBuilder<> builder;
    llvm::Function* fn;
    LoweringTarget& target;
    // Scalar locals/params live in entry-block allocas (mutable: load on
    // read, store on write) so loops and reassignment work. `values` maps a
    // scalar name to its alloca slot; `slotTypes` to the slot element type.
    // Buffer parameters are never reassigned, so their addrspace(1) base
    // pointer is kept directly in `bufferBases` rather than behind an alloca.
    std::map<std::string, llvm::Value*> values;       // scalar name -> alloca slot
    std::map<std::string, llvm::Type*> slotTypes;     // scalar name -> slot elem type
    std::map<std::string, bool> signedness;
    std::map<std::string, llvm::Value*> bufferBases;  // buffer name -> addrspace(1) ptr
    std::map<std::string, llvm::Type*> bufferElems;   // buffer name -> element type
    // Bindless buffer-array params (Buffer<T>[]): the descriptor-array binding +
    // element type per name, plus (CPU) the materialized handle-array base. There
    // is NO single base — `bufs[idx]` selects a descriptor via bufferArrayElement.
    struct BufferArrayInfo { unsigned binding; llvm::Type* elemTy; bool isSigned; };
    std::map<std::string, BufferArrayInfo> bufferArrayBindings;
    std::map<std::string, llvm::Value*> bufferArrayBases;  // CPU handle-array ptr
    // Texture2D / Sampler kernel params (Item 8): the materialized backend
    // handle per name. A `tex.sample(s, u, v)` looks the texture up here and the
    // sampler arg up in samplerHandles, then hands both to target.sampleTexture.
    std::map<std::string, llvm::Value*> textureHandles;  // texture name -> handle
    std::map<std::string, llvm::Type*> textureTexelTypes; // texture name -> texel
                                                          // scalar T (float / i32)
    std::map<std::string, int> textureDims;               // texture name -> 2 or 3
    std::map<std::string, llvm::Value*> imageHandles;    // image name -> storage-image handle
    std::map<std::string, llvm::Value*> samplerHandles;  // sampler name -> descriptor
    // AccelerationStructure kernel params and RayQuery body locals (cajeta-gpu
    // Part C ray query). An AS param's materialized descriptor handle is kept by
    // name; a RayQuery local's opaque alloca (the OpVariable Function) by name.
    // rq.initialize(as, ...) looks the AS up in accelHandles; every RayQuery op
    // takes the alloca from rayQuerySlots.
    std::map<std::string, llvm::Value*> accelHandles;    // AS name -> descriptor
    std::map<std::string, llvm::Value*> rayQuerySlots;   // RayQuery name -> alloca
    // Software ray-query (ray-query-to-core): a RayQuery alloca -> the AS handle
    // (the software BVH `Buffer<float32>` base, an i64 on CPU) recorded at
    // rq.initialize so each rq.proceed() can pass it to the SoftwareRayQuery walk.
    std::map<llvm::Value*, llvm::Value*> rayQueryBvh;
    bool swCursorCached = false;
    DeviceStructInfo swCursorInfoCache;
    // CooperativeMatrix locals (CM4): the alloca slot holds the tile. For the
    // NATIVE tier `matrixType` is the opaque OpTypeCooperativeMatrixKHR device
    // type (loaded/stored as a whole object). For the SOFTWARE tier (CM6) it is
    // a flat `[Rows*Cols x elem]` array and the ops are emitted as a strided
    // gather/scatter + a triple-loop multiply-add; elem/rows/cols/use describe
    // the tile shape for those loops.
    struct CoopMatrixSlot {
        llvm::Value* alloca = nullptr;
        llvm::Type* matrixType = nullptr;   // opaque tile (native) or [N x elem] (software)
        bool software = false;
        llvm::Type* elemType = nullptr;     // device scalar (storage) element type
        uint32_t rows = 0, cols = 0, use = 0;
    };
    std::map<std::string, CoopMatrixSlot> coopMatrixSlots;
    // (dtype,shape) keys already announced via a software-tier note, so the
    // `note: [mma-tiering]` is emitted once per distinct tile, not per use.
    std::set<std::string> notedCoopTiers;

    std::vector<LoweringTarget::KernelParam> kparams;  // admitted params
    bool paramsAsArgs = false;  // true for @Device helpers (params are fn args)
    std::map<std::string, bool> bufferElemSigned;  // buffer name -> elem signed?
    // Set true when the body lowers a cross-lane subgroup op (shuffle/ballot/
    // reduce). Read at kernel finalization to request maximal reconvergence on
    // backends that support it (Vulkan). Per-kernel (DeviceLowerer is per-kernel).
    bool usedSubgroupOp_ = false;
    // POD struct params (Item 7): the materialized aggregate SSA value per param
    // name, plus its field index/type/signedness map. A field read `name.field`
    // is an extractvalue from structValues[name] at the recorded index — no
    // alloca (keeps it valid in SPIR-V logical addressing). Read-only in v1.
    std::map<std::string, llvm::Value*> structValues;       // name -> struct value
    std::map<std::string, DeviceStructInfo> structFields;   // name -> field map
    // @ValueType-typed names (params/locals) -> their CajetaType (S8). Kernel
    // bodies aren't host-type-resolved, so an operand expression's
    // getResolvedType() is null in the device lowerer; this is how a value-type
    // operand of `a OP b` recovers its class to resolve the @Device operator.
    std::map<std::string, CajetaTypePtr> valueTypeNames;
    // @ValueType classes constructible in this body (`new/stack Vec2(...)`),
    // keyed by simple type name (S8 aggregate-returning operators). A
    // value-type-returning @Device operator builds its result by value — an
    // `insertvalue` chain into the device struct — so the lowerer needs the
    // class's layout by the source-written name. Populated from the operand
    // value types and the declaring class.
    std::map<std::string, std::shared_ptr<CajetaClass>> valueTypeCtors;
    // Matrix<T,R,C> locals (B1): name -> (rows, cols). A matrix lives in a
    // `<R*C x T>` slot — identical LLVM type to a Vector<R*C> — so the device
    // walker can't recover the shape from the slot type. This map is how m[r][c]
    // (flat lane r*C+c) and `*` = matmul recover (R,C); a name absent here is
    // NOT a matrix, so all the matrix interceptions are no-ops for vectors.
    std::map<std::string, std::pair<unsigned, unsigned>> matrixShapes;
    // Quaternion local/param names. A quaternion shares the `<4 x T>` slot with
    // a Vector<T,4>; membership here routes `*` to the Hamilton product /
    // rotation and the quaternion methods instead of the element-wise vector path.
    std::set<std::string> quaternionLocals;
    std::shared_ptr<CajetaClass> cls;              // declaring class (helper resolution)
    DeviceFnCache* deviceFns = nullptr;            // shared @Device function cache
    // A dynamic shared array kept TYPED (Vulkan): name -> {array global, array
    // type}. Indexed as gep(arrTy, gv, {0, i}) so the SPIR-V OpTypeArray survives
    // (a spec-constant length needs it), vs the decayed-to-T* base for others.
    std::map<std::string, std::pair<llvm::Value*, llvm::Type*>> arrayShared;
    // At most one dynamic (runtime-sized) shared array per kernel — the
    // extern unsized addrspace(3) region is a single base; multiple would
    // alias (CUDA extern __shared__ / HIP HIP_DYNAMIC_SHARED both single).
    bool emittedDynamicShared = false;

    // continue/break targets for the innermost enclosing loop.
    struct LoopTarget { llvm::BasicBlock* continueBB; llvm::BasicBlock* breakBB; };
    std::vector<LoopTarget> loopTargets;

    // Allocate a slot in the function entry block (so it dominates every use
    // regardless of which loop/branch block is current). The alloca address
    // space is the backend's (0 on NVPTX, 5/private on AMDGPU) — a fork point
    // (cajeta-amd.md §2). Mirrors the host's entry-positioned-IRBuilder idiom.
    llvm::AllocaInst* entryAlloca(llvm::Type* ty, const std::string& name) {
        llvm::BasicBlock& entry = fn->getEntryBlock();
        llvm::IRBuilder<> eb(&entry, entry.begin());
        return eb.CreateAlloca(ty, target.allocaAddressSpace(), nullptr,
                               name + ".slot");
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
        if (auto fs = std::dynamic_pointer_cast<ForStatement>(node)) {
            lowerFor(fs);
            return;
        }
        if (auto ws = std::dynamic_pointer_cast<WhileStatement>(node)) {
            lowerWhile(ws);
            return;
        }
        if (auto ds = std::dynamic_pointer_cast<DoStatement>(node)) {
            lowerDo(ds);
            return;
        }
        if (auto efs = std::dynamic_pointer_cast<EnhancedForStatement>(node)) {
            lowerEnhancedFor(efs);
            return;
        }
        if (auto bs = std::dynamic_pointer_cast<BreakStatement>(node)) {
            if (!bs->getLabel().empty()) unsupported("labeled break");
            if (loopTargets.empty()) unsupported("break outside loop");
            builder.CreateBr(loopTargets.back().breakBB);
            // Trailing (dead) statements need somewhere to land; the
            // enclosing loop's tail-branch fixup terminates this block.
            builder.SetInsertPoint(llvm::BasicBlock::Create(ctx, "after.break", fn));
            return;
        }
        if (auto cs = std::dynamic_pointer_cast<ContinueStatement>(node)) {
            if (!cs->getLabel().empty()) unsupported("labeled continue");
            if (loopTargets.empty()) unsupported("continue outside loop");
            builder.CreateBr(loopTargets.back().continueBB);
            builder.SetInsertPoint(llvm::BasicBlock::Create(ctx, "after.continue", fn));
            return;
        }
        if (auto es = std::dynamic_pointer_cast<ExpressionStatement>(node)) {
            lowerExprStatement(es->getExpression());
            return;
        }
        if (auto rs = std::dynamic_pointer_cast<ReturnStatement>(node)) {
            // @Kernel returns void; a @Device helper returns its value.
            if (fn->getReturnType()->isVoidTy() || !rs->getExpression()) {
                builder.CreateRetVoid();
            } else {
                llvm::Value* v = lowerExpr(rs->getExpression());
                builder.CreateRet(coerceTo(v, fn->getReturnType()));
            }
            return;
        }
        unsupported("statement form in kernel body");
    }

    void lowerLocalDecl(const std::shared_ptr<LocalVariableDeclaration>& lvd) {
        CajetaTypePtr declType = lvd->getType();
        for (auto& vd : lvd->getVariableDeclarators()) {
            if (!vd) continue;
            const std::string& nm = vd->getIdentifier();
            // RayQuery local (Part C): a device-only opaque function-local. The
            // alloca IS the object (an OpVariable Function of OpTypeRayQueryKHR);
            // any `stack RayQuery()` initializer is just the construction and
            // carries no value to store. Backend-gated: rayQueryType throws on a
            // non-Vulkan backend (XPU-N02).
            if (isRayQueryType(declType)) {
                // Software tier (CPU, etc.): the cursor is a concrete SwRayCursor
                // device struct the SoftwareRayQuery walk reads/writes by value.
                // Native tier (Vulkan): the opaque OpTypeRayQueryKHR.
                llvm::Type* rqTy = target.softwareRayQuery()
                    ? (llvm::Type*) swCursorInfo().type
                    : target.rayQueryType(mod);
                if (!rqTy)
                    unsupported("software RayQuery needs cajeta.gpu.core.SwRayCursor");
                rayQuerySlots[nm] = entryAlloca(rqTy, nm);
                continue;
            }
            // CooperativeMatrix local (CM4/CM6): a device-only matrix-core tile.
            // buildCoopMatrixSlot picks the tier — NATIVE (the alloca holds the
            // opaque OpTypeCooperativeMatrixKHR value, ops lower to the backend
            // coop-matrix seams) or SOFTWARE (a flat `[R*C x elem]` tile, ops are
            // a strided gather/scatter + a triple-loop matmul). A `stack
            // CooperativeMatrix<...>()` initializer is just the construction.
            if (isCooperativeMatrixType(declType)) {
                coopMatrixSlots[nm] = buildCoopMatrixSlot(declType, nm);
                continue;
            }
            // S8: a @ValueType local holds a flat aggregate SSA value (NO alloca
            // — an aggregate store to a Function-storage pointer is invalid under
            // SPIR-V logical addressing, the same reason POD-struct params stay
            // SSA). It must have an initializer (`Vec2 c = a + b;`); field reads
            // go through structFieldRead. Read-only in v1 (value types don't
            // mutate — the S3 mutating-operator ban guarantees it).
            if (declType && declType->isValueType()) {
                DeviceStructInfo si = deviceStructInfo(declType, ctx);
                if (si.type) {
                    // The local's own type is constructible in this body — a
                    // kernel that builds a value type directly (`Vec2 a = new
                    // Vec2(...)`) needs it registered before the initializer is
                    // lowered (params alone don't cover a locally-built type).
                    if (auto dc =
                            std::dynamic_pointer_cast<CajetaClass>(declType))
                        valueTypeCtors[declType->getQName()->getTypeName()] = dc;
                    auto init = vd->getInitializer();
                    if (!init || init->getChildren().empty())
                        unsupported("uninitialized @ValueType local '" + nm + "'");
                    auto initExpr = std::dynamic_pointer_cast<Expression>(
                        init->getChildren()[0]);
                    llvm::Value* v = coerceTo(lowerExpr(initExpr), si.type);
                    structValues[nm] = v;
                    structFields[nm] = si;
                    valueTypeNames[nm] = declType;
                    continue;
                }
            }
            llvm::Type* slotTy = deviceScalarType(declType, ctx);
            if (!slotTy) slotTy = deviceVectorType(declType, ctx);  // Vector<T,N>
            // Matrix<T,R,C> local (B1): a `<R*C x T>` slot, plus its (R,C) shape
            // recorded by name so m[r][c] and `*`=matmul can recover it.
            if (!slotTy) {
                if (auto matT = std::dynamic_pointer_cast<CajetaMatrix>(declType)) {
                    slotTy = deviceMatrixType(declType, ctx);
                    matrixShapes[nm] = {matT->getRows(), matT->getCols()};
                }
            }
            // Quaternion<T> local: a `<4 x T>` slot, tracked by name so `*` and
            // the methods route to the quaternion path.
            if (!slotTy) {
                if (std::dynamic_pointer_cast<CajetaQuaternion>(declType)) {
                    slotTy = deviceQuaternionType(declType, ctx);
                    quaternionLocals.insert(nm);
                }
            }
            auto init = vd->getInitializer();
            if (!init || init->getChildren().empty()) {
                // No initializer — reserve the slot; a later assignment fills it.
                if (!slotTy) unsupported("uninitialized local of non-scalar type");
                values[nm] = entryAlloca(slotTy, nm);
                slotTypes[nm] = slotTy;
                signedness[nm] = typeIsSigned(declType);
                continue;
            }
            auto initExpr = std::dynamic_pointer_cast<Expression>(
                init->getChildren()[0]);
            // `Shared<T> tile = shared T[N];` — workgroup-shared memory. The
            // `shared` placement keyword flags the array creation; lower it to a
            // per-block addrspace(3) global instead of a scalar slot.
            if (auto ne = std::dynamic_pointer_cast<NewExpression>(initExpr)) {
                if (ne->getSharedAlloc()) {
                    lowerSharedDecl(nm, declType, ne);
                    continue;
                }
            }
            llvm::Value* v = lowerExpr(initExpr);
            if (!slotTy) slotTy = v->getType();  // infer slot type from initializer
            llvm::Value* slot = entryAlloca(slotTy, nm);
            builder.CreateStore(coerceTo(v, slotTy), slot);
            values[nm] = slot;
            slotTypes[nm] = slotTy;
            signedness[nm] = typeIsSigned(declType);
        }
    }

    // `Shared<T> name = shared T[size];` — workgroup-shared memory. Shared
    // memory is reserved per block (every thread sees the same region), NOT a
    // per-thread alloca, so it lowers to ONE module-level addrspace(3) global;
    // we then register its decayed element pointer in the buffer maps, after
    // which indexing (tile[i]), assignment, and compound-assignment all reuse
    // the existing addrspace-agnostic buffer path (LLVM tracks the address
    // space on the pointer). Two flavors, by whether `size` folds:
    //   - constant size N -> STATIC: an internal [N x T] global (per-block,
    //     reserved by the assembler).
    //   - runtime size     -> DYNAMIC: an external, unsized [0 x T] global.
    //     The byte count comes from the launch config (sharedMemBytes /
    //     groupMemBytes); both runtimes allow one such region/kernel.
    void lowerSharedDecl(const std::string& nm, const CajetaTypePtr& declType,
                         const std::shared_ptr<NewExpression>& ne) {
        // Element type comes from the Shared<T> LHS type argument (T).
        llvm::Type* elemTy = nullptr;
        bool elemSigned = true;
        if (auto cls = std::dynamic_pointer_cast<CajetaClass>(declType)) {
            if (!cls->getTypeArguments().empty()) {
                elemTy = deviceScalarType(cls->getTypeArguments()[0], ctx);
                elemSigned = typeIsSigned(cls->getTypeArguments()[0]);
            }
        }
        if (!elemTy) unsupported("shared local '" + nm +
                                 "' needs a scalar element type (Shared<T>)");

        // Size from the array creator's single size operand.
        auto acr = std::dynamic_pointer_cast<ArrayCreatorRest>(ne->getCreatorRest());
        if (!acr) unsupported("shared local '" + nm +
                              "' must be an array creation: `shared T[size]`");
        if (acr->getChildren().size() != 1) {
            unsupported("shared array '" + nm +
                        "' must have exactly one dimension");
        }
        auto sizeExpr = std::dynamic_pointer_cast<Expression>(
            acr->getChildren()[0]);
        llvm::Value* sizeV = lowerExpr(sizeExpr);  // constant => static path

        // Static [N x T] (internal, undef) vs dynamic [0 x T] (external). The
        // dynamic size value is unused on the device — the launch sizes it —
        // and is left for DCE.
        llvm::GlobalValue::LinkageTypes linkage;
        llvm::Constant* init;
        uint64_t n;
        bool isDynamic = false;
        if (auto* sizeCI = llvm::dyn_cast<llvm::ConstantInt>(sizeV)) {
            n = sizeCI->getZExtValue();
            if (n == 0) unsupported("shared array '" + nm + "' size must be > 0");
            linkage = llvm::GlobalValue::InternalLinkage;
        } else {
            if (emittedDynamicShared) {
                unsupported("at most one dynamic (runtime-sized) shared array "
                            "per kernel; '" + nm + "' is a second");
            }
            emittedDynamicShared = true;
            isDynamic = true;
            if (target.dynamicSharedNeedsConcreteSize()) {
                // H12: the runtime computes the dynamic-shared length spec constant
                // as sharedBytes/4 — a hardcoded 4-byte element. Until that carries
                // the real element size, reject a non-4-byte element rather than
                // silently mis-size the array (e.g. Shared<half> -> OOB indices in
                // [len/2, len); Shared<double> -> over-allocation + wrong count).
                uint64_t elemBytes = mod.getDataLayout().getTypeAllocSize(elemTy);
                if (elemBytes != 4)
                    unsupported("dynamic Shared<T> currently requires a 4-byte "
                                "element (the runtime's shared-length spec constant "
                                "assumes 4 bytes); got a " +
                                std::to_string(elemBytes) + "-byte element");
                // Vulkan: a concrete INTERNAL [N x T] placeholder (N>1 so it
                // stays an OpTypeArray, not a decayed scalar); the SPIR-V
                // post-emit pass rewrites its length to a spec constant the
                // launch's sharedBytes sets at pipeline creation. N is just the
                // spec constant's default.
                n = 256;
                linkage = llvm::GlobalValue::InternalLinkage;
            } else {
                n = 0;                                  // native extern shared
                linkage = llvm::GlobalValue::ExternalLinkage;
            }
        }
        llvm::ArrayType* arrTy = llvm::ArrayType::get(elemTy, n);
        init = (linkage == llvm::GlobalValue::ExternalLinkage)
                   ? nullptr                            // external: no initializer
                   : (llvm::Constant*) llvm::UndefValue::get(arrTy);
        // '_' not '.': the device global's name becomes a target symbol, and '.'
        // is a directive separator in PTX/AMDGCN asm. A concrete dynamic-shared
        // array (Vulkan) is prefixed so the SPIR-V pass finds it by OpName.
        std::string gname = fn->getName().str() + "_" + nm;
        if (isDynamic && linkage == llvm::GlobalValue::InternalLinkage)
            gname = "cajeta_dynsh_" + gname;
        auto* gv = new llvm::GlobalVariable(
            mod, arrTy, /*isConstant=*/false, linkage, init,
            gname, /*InsertBefore=*/nullptr,
            llvm::GlobalValue::NotThreadLocal, kSharedAS);
        gv->setAlignment(llvm::MaybeAlign(16));

        if (isDynamic && linkage == llvm::GlobalValue::InternalLinkage) {
            // Vulkan dynamic shared: keep the array TYPED (don't decay to T*), so
            // its OpTypeArray survives for the spec-constant length patch.
            arrayShared[nm] = {gv, arrTy};
            bufferElems[nm] = elemTy;
            bufferElemSigned[nm] = elemSigned;
            return;
        }
        // Decay [n x T]* -> T* (addrspace 3) and register like a buffer base so
        // tile[i] reuses lowerLValueAddr's GEP/load/store path unchanged.
        llvm::Value* zero =
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), 0);
        llvm::Value* base = builder.CreateGEP(arrTy, gv, {zero, zero},
                                              nm + ".base");
        bufferBases[nm] = base;
        bufferElems[nm] = elemTy;
        bufferElemSigned[nm] = elemSigned;
    }

    // Coerce any scalar to an i1 truth value (for conditions).
    llvm::Value* toI1(llvm::Value* v) {
        if (v->getType()->isIntegerTy(1)) return v;
        if (v->getType()->isFloatingPointTy())
            return builder.CreateFCmpONE(
                v, llvm::ConstantFP::get(v->getType(), 0.0));
        return builder.CreateICmpNE(
            v, llvm::ConstantInt::get(v->getType(), 0));
    }

    void lowerIf(const std::shared_ptr<IfStatement>& ifs) {
        llvm::Value* cond = toI1(lowerExpr(ifs->getCondition()));
        auto* thenBB = llvm::BasicBlock::Create(ctx, "if.then", fn);
        auto* endBB  = llvm::BasicBlock::Create(ctx, "if.end", fn);
        llvm::BasicBlock* elseBB =
            ifs->getElseBranch() ? llvm::BasicBlock::Create(ctx, "if.else", fn)
                                 : endBB;
        builder.CreateCondBr(cond, thenBB, elseBB);

        builder.SetInsertPoint(thenBB);
        lowerStatement(ifs->getThenBranch());
        if (!builder.GetInsertBlock()->hasTerminator()) builder.CreateBr(endBB);

        if (ifs->getElseBranch()) {
            builder.SetInsertPoint(elseBB);
            lowerStatement(ifs->getElseBranch());
            if (!builder.GetInsertBlock()->hasTerminator()) builder.CreateBr(endBB);
        }
        builder.SetInsertPoint(endBB);
    }

    // for (init; cond; update) body — BB shape mirrors the host
    // (Statement.cpp): head(cond) / body / update / exit; continue→update.
    void lowerFor(const std::shared_ptr<ForStatement>& fs) {
        if (fs->getInit()) lowerStatement(fs->getInit());
        auto* head = llvm::BasicBlock::Create(ctx, "for.head", fn);
        auto* body = llvm::BasicBlock::Create(ctx, "for.body", fn);
        auto* upd  = llvm::BasicBlock::Create(ctx, "for.update", fn);
        auto* exit = llvm::BasicBlock::Create(ctx, "for.exit", fn);
        builder.CreateBr(head);
        builder.SetInsertPoint(head);
        llvm::Value* cond = fs->getCondition()
            ? toI1(lowerExpr(fs->getCondition()))
            : llvm::ConstantInt::getTrue(ctx);   // null cond ⇒ always-true
        builder.CreateCondBr(cond, body, exit);
        builder.SetInsertPoint(body);
        loopTargets.push_back({upd, exit});
        lowerStatement(fs->getBody());
        loopTargets.pop_back();
        if (!builder.GetInsertBlock()->hasTerminator()) builder.CreateBr(upd);
        builder.SetInsertPoint(upd);
        // Update exprs are statement-like (e.g. `j += stride`, `i++`): route
        // through lowerExprStatement so assignments hit lowerAssign.
        for (auto& u : fs->getUpdate()) lowerExprStatement(u);
        builder.CreateBr(head);
        builder.SetInsertPoint(exit);
    }

    // Grid-stride for-each (Item 6):
    //   for (idxType idx, elemType elem : buf.range(count)) body
    // ⇒ for (idx = globalId.x; idx < count; idx += gridSize.x) {
    //        elem = buf[idx]; body
    //    }
    // The iterable MUST be `<bufferParam>.range(<count>)` — device buffers carry
    // no length, so the count is explicit (as in every GPU language). The element
    // binding is a value copy of buf[idx] (range-for semantics); writes go via the
    // index binding (`buf[idx] = …`). The iterator (index) binding is optional;
    // without it the body can read `elem` but has no index to write by.
    void lowerEnhancedFor(const std::shared_ptr<EnhancedForStatement>& efs) {
        auto mc = std::dynamic_pointer_cast<MethodCallExpression>(
            efs->getIterableExpr());
        std::string bufName;
        if (mc && mc->getMethodCallName() == "range" &&
            !mc->getChildren().empty()) {
            if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(
                    mc->getChildren()[0]))
                bufName = id->getTextValue();
        }
        if (bufName.empty()) {
            throw cajeta::Exception(
                "XPU kernel lowering: a for-each in a kernel must iterate "
                "`buffer.range(count)` (device buffers are unsized)", "XPU-N02");
        }
        auto bv = bufferBases.find(bufName);
        auto be = bufferElems.find(bufName);
        if (bv == bufferBases.end() || be == bufferElems.end()) {
            throw cajeta::Exception(
                "XPU kernel lowering: for-each receiver '" + bufName +
                "' is not a kernel buffer parameter", "XPU-N02");
        }
        if (mc->getParameters().size() != 1) {
            throw cajeta::Exception(
                "XPU kernel lowering: buffer.range(count) takes exactly one "
                "argument", "XPU-N02");
        }

        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);

        // Index type follows the iterator binding (default i32 / uint).
        llvm::Type* idxTy =
            efs->getIteratorType() ? deviceScalarType(efs->getIteratorType(), ctx)
                                   : i32;
        if (!idxTy || !idxTy->isIntegerTy()) idxTy = i32;
        bool idxSigned =
            efs->getIteratorType() && typeIsSigned(efs->getIteratorType());

        llvm::Value* count =
            coerceTo(lowerExpr(mc->getParameters()[0].expression), idxTy);
        llvm::Value* stride = coerceTo(target.gridSize(builder, mod, 0), idxTy);

        // Index slot, initialized to the global x-id. Bound to the iterator name
        // when present (so the body can `buf[idx] = …`).
        const bool hasIdx = efs->getIteratorType() != nullptr;
        std::string idxName = hasIdx ? efs->getIteratorName()
                                     : (bufName + ".fe.idx");
        llvm::Value* idxSlot = entryAlloca(idxTy, idxName);
        builder.CreateStore(coerceTo(target.globalId(builder, mod, 0), idxTy),
                            idxSlot);
        if (hasIdx) {
            values[idxName] = idxSlot;
            slotTypes[idxName] = idxTy;
            signedness[idxName] = idxSigned;
        }

        // Element binding: a per-iteration value copy of buf[idx].
        llvm::Type* elemTy = be->second;
        const std::string& elemName = efs->getElementName();
        llvm::Value* elemSlot = entryAlloca(elemTy, elemName);
        values[elemName] = elemSlot;
        slotTypes[elemName] = elemTy;
        auto sit = bufferElemSigned.find(bufName);
        signedness[elemName] = sit != bufferElemSigned.end() ? sit->second : true;

        auto* head = llvm::BasicBlock::Create(ctx, "fe.head", fn);
        auto* body = llvm::BasicBlock::Create(ctx, "fe.body", fn);
        auto* upd  = llvm::BasicBlock::Create(ctx, "fe.update", fn);
        auto* exit = llvm::BasicBlock::Create(ctx, "fe.exit", fn);
        builder.CreateBr(head);

        builder.SetInsertPoint(head);
        llvm::Value* i = builder.CreateLoad(idxTy, idxSlot, idxName);
        // Thread indices are non-negative; an unsigned compare is correct and
        // lets a huge `count` (near INT_MAX) work.
        builder.CreateCondBr(builder.CreateICmpULT(i, count, "fe.cmp"),
                             body, exit);

        builder.SetInsertPoint(body);
        // elem = buf[idx]  (widen idx to i64 for the element GEP, like array idx).
        llvm::Value* i64idx =
            builder.CreateIntCast(i, i64, /*isSigned=*/idxSigned);
        llvm::Value* addr =
            target.bufferElementPtr(builder, mod, bv->second, elemTy, i64idx);
        builder.CreateStore(builder.CreateLoad(elemTy, addr, "fe.elem"),
                            elemSlot);
        loopTargets.push_back({upd, exit});
        lowerStatement(efs->getBody());
        loopTargets.pop_back();
        if (!builder.GetInsertBlock()->hasTerminator()) builder.CreateBr(upd);

        builder.SetInsertPoint(upd);
        llvm::Value* next = builder.CreateAdd(
            builder.CreateLoad(idxTy, idxSlot, idxName), stride, "fe.next");
        builder.CreateStore(next, idxSlot);
        builder.CreateBr(head);

        builder.SetInsertPoint(exit);
    }

    // while (cond) body — head(cond) / body / exit; continue→head.
    void lowerWhile(const std::shared_ptr<WhileStatement>& ws) {
        auto* head = llvm::BasicBlock::Create(ctx, "while.head", fn);
        auto* body = llvm::BasicBlock::Create(ctx, "while.body", fn);
        auto* exit = llvm::BasicBlock::Create(ctx, "while.exit", fn);
        builder.CreateBr(head);
        builder.SetInsertPoint(head);
        builder.CreateCondBr(toI1(lowerExpr(ws->getCondition())), body, exit);
        builder.SetInsertPoint(body);
        loopTargets.push_back({head, exit});
        lowerStatement(ws->getBody());
        loopTargets.pop_back();
        if (!builder.GetInsertBlock()->hasTerminator()) builder.CreateBr(head);
        builder.SetInsertPoint(exit);
    }

    // do body while (cond) — body / tail(cond) / exit; continue→tail.
    void lowerDo(const std::shared_ptr<DoStatement>& ds) {
        auto* body = llvm::BasicBlock::Create(ctx, "do.body", fn);
        auto* tail = llvm::BasicBlock::Create(ctx, "do.tail", fn);
        auto* exit = llvm::BasicBlock::Create(ctx, "do.exit", fn);
        builder.CreateBr(body);
        builder.SetInsertPoint(body);
        loopTargets.push_back({tail, exit});
        lowerStatement(ds->getBody());
        loopTargets.pop_back();
        if (!builder.GetInsertBlock()->hasTerminator()) builder.CreateBr(tail);
        builder.SetInsertPoint(tail);
        builder.CreateCondBr(toI1(lowerExpr(ds->getCondition())), body, exit);
        builder.SetInsertPoint(exit);
    }

    void lowerExprStatement(const ExpressionPtr& expr) {
        if (auto bin = std::dynamic_pointer_cast<BinaryOpExpression>(expr)) {
            if (bin->isAssignment()) { lowerAssign(bin); return; }
        }
        // Bare expression (builtin call, `i++` for side effects, …) — lower
        // and discard the value.
        lowerExpr(expr);
    }

    // `lhs = rhs` (plain) and `lhs op= rhs` (compound: load-op-store). Works on
    // both scalar locals and buffer elements via lowerLValueAddr.
    void lowerAssign(const std::shared_ptr<BinaryOpExpression>& bin) {
        ExpressionPtr lhs = exprChild(bin, 0);
        ExpressionPtr rhs = exprChild(bin, 1);
        // A lane of a vector local (`v.x = …` / `v[i] = …`) isn't addressable —
        // it's load-insertelement-store, not a GEP. Handled here before the
        // l-value-address path (which only knows scalars and buffers).
        if (tryMatrixElementAssign(bin, lhs, rhs)) return;  // m[r][c] = … (B1)
        if (tryVectorElementAssign(bin, lhs, rhs)) return;
        auto [addr, elemTy] = lowerLValueAddr(lhs);
        llvm::Value* rv = lowerExpr(rhs);
        BinaryOp op = bin->getBinaryOp();
        if (op != BINARY_OP_ASSIGN) {
            llvm::Value* cur = builder.CreateLoad(elemTy, addr, "cur");
            rv = applyBinOp(compoundBase(op), cur, rv, lvalueSigned(lhs),
                            elemTy->isFloatingPointTy());
        }
        builder.CreateStore(coerceTo(rv, elemTy), addr);
    }

    static BinaryOp compoundBase(BinaryOp op) {
        switch (op) {
            case BINARY_OP_ADD_EQUALS: return BINARY_OP_ADD;
            case BINARY_OP_SUB_EQUALS: return BINARY_OP_SUB;
            case BINARY_OP_MUL_EQUALS: return BINARY_OP_MUL;
            case BINARY_OP_DIV_EQUALS: return BINARY_OP_DIV;
            case BINARY_OP_MOD_EQUALS: return BINARY_OP_MOD;
            case BINARY_OP_BITAND_EQUALS: return BINARY_OP_BITAND;
            case BINARY_OP_BITOR_EQUALS:  return BINARY_OP_BITOR;
            case BINARY_OP_BITXOR_EQUALS: return BINARY_OP_BITXOR;
            case BINARY_OP_SHIFTLEFT_EQUALS:   return BINARY_OP_SHIFTLEFT;
            case BINARY_OP_SHIFTRIGHT_EQUALS:  return BINARY_OP_SHIFTRIGHT;
            case BINARY_OP_USHIFTRIGHT_EQUALS: return BINARY_OP_USHIFTRIGHT;
            default: return BINARY_OP_ADD;  // unreachable (caller gated)
        }
    }

    bool lvalueSigned(const ExpressionPtr& e) {
        if (auto* f = structFieldOf(e)) return f->isSigned;
        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(e)) {
            auto it = signedness.find(id->getTextValue());
            return it != signedness.end() ? it->second : true;
        }
        if (auto ai = std::dynamic_pointer_cast<ArrayIndexExpression>(e)) {
            if (auto b = std::dynamic_pointer_cast<IdentifierExpression>(
                    exprChild(ai, 0))) {
                auto it = bufferElemSigned.find(b->getTextValue());
                return it != bufferElemSigned.end() ? it->second : true;
            }
        }
        return true;
    }

    // ---- expressions ----------------------------------------------------

    llvm::Value* lowerExpr(const ExpressionPtr& expr) {
        if (!expr) unsupported("null expression");

        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(expr)) {
            const std::string& nm = id->getTextValue();
            auto bb = bufferBases.find(nm);
            if (bb != bufferBases.end()) return bb->second;  // buffer base ptr
            auto it = values.find(nm);
            if (it != values.end())
                return builder.CreateLoad(slotTypes[nm], it->second, nm);  // load slot
            // Whole POD/@ValueType param read by name (S8): the materialized
            // aggregate SSA value (no alloca — extractvalue-only, SPIR-V-safe).
            auto sv = structValues.find(nm);
            if (sv != structValues.end()) return sv->second;
            unsupported("unbound identifier '" + nm + "'");
        }
        if (auto il = std::dynamic_pointer_cast<IntegerLiteralExpression>(expr)) {
            // Mirror the host literal lowering (LiteralExpression.cpp): honor the
            // radix (hex/bin/oct), strip the prefix / trailing `L` / digit-group
            // underscores, and parse via APInt — never std::stoll, which reads the
            // wrong base, stops at `_`, and *throws* on overflow (crashing the
            // compiler). Materialize at the literal's resolved width when known
            // (so int64/L literals aren't truncated), else the i32 default; coerceTo
            // narrows at the use site. Fixes H13-H16/L1.
            uint8_t radix; size_t prefixLen = 0;
            switch (il->getIntegerLiteralType()) {
                case INTEGER_LITERAL_TYPE_BINARY: radix = 2;  prefixLen = 2; break;
                case INTEGER_LITERAL_TYPE_OCT:    radix = 8;  prefixLen = 0; break;
                case INTEGER_LITERAL_TYPE_HEX:    radix = 16; prefixLen = 2; break;
                default:                          radix = 10; prefixLen = 0; break;
            }
            std::string text = il->getRawValue();
            if (prefixLen && text.size() >= prefixLen) text.erase(0, prefixLen);
            bool hasLSuffix = !text.empty() && (text.back() == 'l' || text.back() == 'L');
            if (hasLSuffix) text.pop_back();
            text.erase(std::remove(text.begin(), text.end(), '_'), text.end());
            if (text.empty()) text = "0";
            llvm::APInt full(64, text, radix);
            // Default i32 (the kernel norm); widen to i64 when the resolved type is
            // int64, the literal carries an `L` suffix, or the value simply needs
            // more than 32 bits — otherwise it would be silently truncated (H15).
            unsigned width = 32;
            if (il->getResolvedType())
                if (llvm::Type* rt = deviceScalarType(il->getResolvedType(), ctx))
                    if (rt->isIntegerTy()) width = rt->getIntegerBitWidth();
            if (hasLSuffix || full.getActiveBits() > 32) width = 64;
            return llvm::ConstantInt::get(ctx, full.zextOrTrunc(width));
        }
        if (auto fl = std::dynamic_pointer_cast<FloatLiteralExpression>(expr)) {
            // Mirror the host: parse via APFloat (no std::stod overflow crash) and
            // pick f32 vs f64 by suffix/resolved type instead of always f32 — a
            // `double` literal otherwise loses its low bits (parsed then re-widened
            // from an f32-rounded value). Default stays f32 (the device norm) so
            // bare kernel literals don't silently become f64. Fixes H16/L1.
            std::string text = fl->getRawValue();
            bool wantF32 = true;
            if (!text.empty()) {
                char last = text.back();
                if (last == 'd' || last == 'D') wantF32 = false;
                else if (last == 'f' || last == 'F') wantF32 = true;
                else if (fl->getResolvedType())
                    if (llvm::Type* rt = deviceScalarType(fl->getResolvedType(), ctx))
                        wantF32 = rt->isFloatTy();
                if (last=='f'||last=='F'||last=='d'||last=='D') text.pop_back();
            }
            const llvm::fltSemantics& sem = wantF32 ? llvm::APFloat::IEEEsingle()
                                                    : llvm::APFloat::IEEEdouble();
            llvm::APFloat apf(sem);
            if (!apf.convertFromString(text, llvm::APFloat::rmNearestTiesToEven))
                return llvm::ConstantFP::getZero(
                    wantF32 ? llvm::Type::getFloatTy(ctx)
                            : llvm::Type::getDoubleTy(ctx));
            return llvm::ConstantFP::get(ctx, apf);
        }
        // Boolean literal (`true` / `false`) → i1. Booleans are a
        // TextLiteralExpression (LITERAL_TYPE_BOOL), distinct from the
        // Integer/Float literal nodes above; device code that returns a bool
        // (e.g. SoftwareRayQuery.slabHit) needs them.
        if (auto tl = std::dynamic_pointer_cast<TextLiteralExpression>(expr)) {
            if (tl->getLiteralType() == LITERAL_TYPE_BOOL) {
                return llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx),
                                              tl->getRawValue() == "true" ? 1 : 0);
            }
            unsupported("text/string literal in kernel body");
        }
        if (auto ne = std::dynamic_pointer_cast<NewExpression>(expr)) {
            // A value-type-returning @Device operator builds its result by value
            // (S8): `new/stack Vec2(...)` -> SSA aggregate. Tried before Vector so
            // a user value type can't be mistaken for the builtin.
            if (llvm::Value* vt = lowerNewValueType(ne)) return vt;
            // Built-in Matrix<T,R,C> construction -> SSA `<R*C x T>` (B1).
            if (llvm::Value* mt = lowerNewMatrix(ne)) return mt;
            // Built-in Quaternion<T> construction -> SSA `<4 x T>` (w, x, y, z).
            if (llvm::Value* qt = lowerNewQuaternion(ne)) return qt;
            // Built-in Vector<T,N> construction -> SSA `<N x T>` (no alloc).
            return lowerNewVector(ne);
        }
        if (auto mc = std::dynamic_pointer_cast<MethodCallExpression>(expr)) {
            return lowerBuiltinCall(mc);
        }
        if (auto ai = std::dynamic_pointer_cast<ArrayIndexExpression>(expr)) {
            // Matrix local `m[r][c]` reads element flat lane r*C+c (B1) — tried
            // before the vector path since m[r] is a row, not a flat lane.
            if (llvm::Value* me = matrixIndexRead(ai)) return me;
            // Vector local `v[i]` reads a lane (extractelement), not memory.
            if (llvm::Value* ve = vectorIndexRead(ai)) return ve;
            auto [addr, elemTy] = lowerLValueAddr(ai);
            return builder.CreateLoad(elemTy, addr, "elem");
        }
        if (auto dot = std::dynamic_pointer_cast<DotExpression>(expr)) {
            // POD-struct field read `name.field` (Item 7).
            if (llvm::Value* fv = structFieldRead(expr)) return fv;
            // Vector component read `v.x` / `v.r` (extractelement).
            if (llvm::Value* cv = vectorComponentRead(dot)) return cv;
            unsupported("field access — only POD-struct kernel params "
                        "support 'name.field'");
        }
        if (auto bin = std::dynamic_pointer_cast<BinaryOpExpression>(expr)) {
            return lowerBinaryOp(bin);
        }
        if (auto pre = std::dynamic_pointer_cast<PrefixExpression>(expr)) {
            return lowerPrefix(pre);
        }
        if (auto post = std::dynamic_pointer_cast<PostfixExpression>(expr)) {
            return lowerPostfix(post);
        }
        if (auto cast = std::dynamic_pointer_cast<CastExpression>(expr)) {
            llvm::Value* v = lowerExpr(exprChild(cast, 0));
            // Prefer the declared target type; resolvedType may be unset since
            // the device lowerer walks the kernel AST without running
            // resolveTypes (the host stub body is what gets resolved).
            CajetaTypePtr ct = cast->getResolvedType();
            if (!ct) ct = cast->getDestType();
            llvm::Type* dst = deviceScalarType(ct, ctx);
            if (!dst) unsupported("cast to non-scalar type");
            return castNumeric(v, dst, typeIsSigned(ct),
                               exprSigned(exprChild(cast, 0)));
        }
        unsupported("expression form in kernel body");
    }

    llvm::Value* lowerPrefix(const std::shared_ptr<PrefixExpression>& pre) {
        ExpressionPtr operand = exprChild(pre, 0);
        switch (pre->getOp()) {
            case PREFIX_OP_POSITIVE:
                return lowerExpr(operand);
            case PREFIX_OP_NEGATIVE: {
                llvm::Value* v = lowerExpr(operand);
                return v->getType()->isFloatingPointTy() ? builder.CreateFNeg(v)
                                                          : builder.CreateNeg(v);
            }
            case PREFIX_OP_BITNOT:
                return builder.CreateNot(lowerExpr(operand));
            case PREFIX_OP_LOGNOT:
                return builder.CreateXor(toI1(lowerExpr(operand)),
                                         llvm::ConstantInt::getTrue(ctx));
            case PREFIX_OP_INC:
            case PREFIX_OP_DEC:
                return lowerIncDec(operand, pre->getOp() == PREFIX_OP_INC,
                                   /*returnOld=*/false);
        }
        unsupported("prefix operator");
    }

    llvm::Value* lowerPostfix(const std::shared_ptr<PostfixExpression>& post) {
        return lowerIncDec(exprChild(post, 0),
                           post->getOp() == POSTFIX_OP_INC, /*returnOld=*/true);
    }

    // Shared ++/-- on an l-value: load, ±1, store. returnOld picks postfix
    // (old value) vs prefix (new value) semantics.
    llvm::Value* lowerIncDec(const ExpressionPtr& operand, bool inc,
                             bool returnOld) {
        auto [addr, ty] = lowerLValueAddr(operand);
        llvm::Value* cur = builder.CreateLoad(ty, addr, "cur");
        bool fp = ty->isFloatingPointTy();
        llvm::Value* one = fp ? (llvm::Value*) llvm::ConstantFP::get(ty, 1.0)
                              : (llvm::Value*) llvm::ConstantInt::get(ty, 1);
        llvm::Value* nv = applyBinOp(inc ? BINARY_OP_ADD : BINARY_OP_SUB,
                                     cur, one, lvalueSigned(operand), fp);
        builder.CreateStore(nv, addr);
        return returnOld ? cur : nv;
    }

    llvm::Value* castNumeric(llvm::Value* v, llvm::Type* dst, bool dstSigned,
                             bool srcSigned) {
        llvm::Type* src = v->getType();
        if (src == dst) return v;
        bool sFp = src->isFloatingPointTy(), dFp = dst->isFloatingPointTy();
        if (sFp && dFp)   return builder.CreateFPCast(v, dst);
        if (!sFp && !dFp) return builder.CreateIntCast(v, dst, srcSigned);
        if (!sFp && dFp)  return srcSigned ? builder.CreateSIToFP(v, dst)
                                           : builder.CreateUIToFP(v, dst);
        return dstSigned ? builder.CreateFPToSI(v, dst)   // fp -> int
                         : builder.CreateFPToUI(v, dst);
    }

    // ---- Vector<T,N> (mirror of the host expression codegen) ------------
    //
    // The device walker has no resolved types, so a vector local is recognized
    // by its slot type being an LLVM vector (`<N x T>`). Construction reads the
    // element type + lane count straight off the NewExpression's captured type
    // arguments. All IR is built through the shared vecops helpers, so the lane
    // mapping / dot-reduce logic stays identical to the host path.

    // `new/stack Vec2(f0, f1, ...)` for a @ValueType constructible in this body
    // (S8) -> SSA aggregate built by an `insertvalue` chain into the device
    // struct. Returns nullptr when the type name isn't a known value type (so the
    // caller falls through to Vector). v1 maps constructor arguments positionally
    // to fields in declaration order — the shape every @ValueType POD's canonical
    // constructor has (`Vec2(x, y){ this.x=x; this.y=y; }`), mirroring how
    // lowerNewVector maps positional lanes; a reordering/computing constructor is
    // out of scope (the operators are interception placeholders).
    llvm::Value* lowerNewValueType(const std::shared_ptr<NewExpression>& ne) {
        auto cit = valueTypeCtors.find(ne->getTypeName());
        if (cit == valueTypeCtors.end()) return nullptr;
        DeviceStructInfo si = deviceStructInfo(cit->second, ctx);
        if (!si.type) return nullptr;
        auto ccr = std::dynamic_pointer_cast<ClassCreatorRest>(
            ne->getCreatorRest());
        if (!ccr)
            unsupported("@ValueType '" + ne->getTypeName() +
                        "' construction needs a (a, b, ...) argument list");
        const auto& params = ccr->getParameters();
        unsigned nfields = si.type->getNumElements();
        if (params.size() != nfields)
            unsupported("@ValueType '" + ne->getTypeName() + "' needs " +
                        std::to_string(nfields) + " constructor arguments (got " +
                        std::to_string(params.size()) + ")");
        llvm::Value* agg = llvm::UndefValue::get(si.type);
        for (unsigned i = 0; i < nfields; ++i) {
            llvm::Value* fv = coerceTo(lowerExpr(params[i].expression),
                                       si.type->getElementType(i));
            agg = builder.CreateInsertValue(agg, fv, {i});
        }
        return agg;
    }

    // `new Matrix<T,R,C>(e00, e01, ...)` -> SSA `<R*C x T>` row-major (B1).
    // Returns nullptr when the `new` isn't a Matrix so the caller falls through
    // to Vector. R*C scalar arguments fill the matrix row by row.
    llvm::Value* lowerNewMatrix(const std::shared_ptr<NewExpression>& ne) {
        const auto& targs = ne->getTypeArguments();
        if (ne->getTypeName() != "Matrix" || targs.size() != 3) return nullptr;
        llvm::Type* elemTy = deviceScalarType(targs[0], ctx);
        auto cR = std::dynamic_pointer_cast<CajetaConstantType>(targs[1]);
        auto cC = std::dynamic_pointer_cast<CajetaConstantType>(targs[2]);
        if (!elemTy || !cR || !cC)
            unsupported("invalid Matrix<T,R,C> type arguments");
        unsigned rows = (unsigned) cR->getValue();
        unsigned cols = (unsigned) cC->getValue();
        auto ccr = std::dynamic_pointer_cast<ClassCreatorRest>(
            ne->getCreatorRest());
        if (!ccr) unsupported("Matrix construction requires an argument list");
        const auto& params = ccr->getParameters();
        if (params.size() != rows * cols)
            unsupported("Matrix<...," + std::to_string(rows) + "," +
                        std::to_string(cols) + "> needs " +
                        std::to_string(rows * cols) + " arguments (got " +
                        std::to_string(params.size()) + ")");
        std::vector<llvm::Value*> elems;
        elems.reserve(rows * cols);
        for (auto& p : params)
            elems.push_back(vecops::coerceScalar(
                builder, lowerExpr(p.expression), elemTy));
        return matops::buildMatrix(builder, elemTy, rows, cols, elems);
    }

    // Built-in Quaternion<T> construction -> `<4 x T>` (w, x, y, z).
    llvm::Value* lowerNewQuaternion(const std::shared_ptr<NewExpression>& ne) {
        const auto& targs = ne->getTypeArguments();
        if (ne->getTypeName() != "Quaternion" || targs.size() != 1) return nullptr;
        llvm::Type* elemTy = deviceScalarType(targs[0], ctx);
        if (!elemTy) unsupported("invalid Quaternion<T> element type");
        auto ccr = std::dynamic_pointer_cast<ClassCreatorRest>(
            ne->getCreatorRest());
        if (!ccr || ccr->getParameters().size() != 4)
            unsupported("Quaternion<T> needs 4 arguments (w, x, y, z)");
        std::vector<llvm::Value*> elems;
        elems.reserve(4);
        for (auto& p : ccr->getParameters())
            elems.push_back(vecops::coerceScalar(
                builder, lowerExpr(p.expression), elemTy));
        return vecops::buildVector(builder, elemTy, 4, elems);
    }

    // m[r][c] read on a matrix local: the LHS is ArrayIndex(ArrayIndex(m, r), c)
    // with m in matrixShapes. Loads the slot and extractelement at flat lane
    // r*C+c. Returns nullptr when `ai` isn't that nested matrix shape (so the
    // vector/buffer path runs). Must be tried before vectorIndexRead — m[r] is
    // a row, not a flat lane.
    llvm::Value* matrixIndexRead(const std::shared_ptr<ArrayIndexExpression>& ai) {
        auto inner = std::dynamic_pointer_cast<ArrayIndexExpression>(
            exprChild(ai, 0));
        if (!inner) return nullptr;
        auto baseId = std::dynamic_pointer_cast<IdentifierExpression>(
            exprChild(inner, 0));
        if (!baseId) return nullptr;
        auto sh = matrixShapes.find(baseId->getTextValue());
        if (sh == matrixShapes.end()) return nullptr;
        unsigned cols = sh->second.second;
        llvm::Type* slotTy = slotTypes[baseId->getTextValue()];
        llvm::Value* m = builder.CreateLoad(
            slotTy, values[baseId->getTextValue()], baseId->getTextValue());
        llvm::Value* r = toI32(lowerExpr(exprChild(inner, 1)));
        llvm::Value* c = toI32(lowerExpr(exprChild(ai, 1)));
        return matops::getElement(builder, m, sh->second.first, cols, r, c);
    }

    // m[r][c] = v on a matrix local: load slot, insertelement at flat lane
    // r*C+c, store back. Returns false when `lhs` isn't a nested matrix index.
    bool tryMatrixElementAssign(const std::shared_ptr<BinaryOpExpression>& bin,
                                const ExpressionPtr& lhs,
                                const ExpressionPtr& rhs) {
        auto outer = std::dynamic_pointer_cast<ArrayIndexExpression>(lhs);
        if (!outer) return false;
        auto inner = std::dynamic_pointer_cast<ArrayIndexExpression>(
            exprChild(outer, 0));
        if (!inner) return false;
        auto baseId = std::dynamic_pointer_cast<IdentifierExpression>(
            exprChild(inner, 0));
        if (!baseId) return false;
        auto sh = matrixShapes.find(baseId->getTextValue());
        if (sh == matrixShapes.end()) return false;
        if (bin->getBinaryOp() != BINARY_OP_ASSIGN)
            unsupported("compound assignment to a matrix element");
        unsigned cols = sh->second.second;
        const std::string& nm = baseId->getTextValue();
        llvm::Type* slotTy = slotTypes[nm];
        auto* vecTy = llvm::cast<llvm::FixedVectorType>(slotTy);
        llvm::Value* slot = values[nm];
        llvm::Value* cur = builder.CreateLoad(slotTy, slot, nm);
        llvm::Value* r = toI32(lowerExpr(exprChild(inner, 1)));
        llvm::Value* c = toI32(lowerExpr(exprChild(outer, 1)));
        llvm::Value* rv = coerceTo(lowerExpr(rhs), vecTy->getElementType());
        builder.CreateStore(
            matops::setElement(builder, cur, sh->second.first, cols, r, c, rv),
            slot);
        return true;
    }

    // Widen/narrow an integer index to i32 for matrix lane arithmetic.
    llvm::Value* toI32(llvm::Value* v) {
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        if (v->getType() == i32) return v;
        return builder.CreateIntCast(v, i32, /*isSigned=*/false, "mat.idx");
    }

    // `new Vector<T,N>(a, b, ...)` -> SSA `<N x T>`. Rejects any other `new`.
    llvm::Value* lowerNewVector(const std::shared_ptr<NewExpression>& ne) {
        const auto& targs = ne->getTypeArguments();
        if (ne->getTypeName() != "Vector" || targs.size() != 2) {
            unsupported("`new` in a kernel body (only Vector<T,N> construction "
                        "is supported)");
        }
        llvm::Type* elemTy = deviceScalarType(targs[0], ctx);
        auto cN = std::dynamic_pointer_cast<CajetaConstantType>(targs[1]);
        if (!elemTy || !cN) unsupported("invalid Vector<T,N> type arguments");
        unsigned lanes = (unsigned) cN->getValue();
        auto ccr = std::dynamic_pointer_cast<ClassCreatorRest>(
            ne->getCreatorRest());
        if (!ccr) {
            unsupported("Vector construction requires a (a, b, ...) argument "
                        "list");
        }
        const auto& params = ccr->getParameters();
        if (params.size() != lanes) {
            unsupported("Vector<...," + std::to_string(lanes) + "> needs " +
                        std::to_string(lanes) + " arguments (got " +
                        std::to_string(params.size()) + ")");
        }
        std::vector<llvm::Value*> elems;
        elems.reserve(lanes);
        for (auto& p : params) {
            elems.push_back(vecops::coerceScalar(
                builder, lowerExpr(p.expression), elemTy));
        }
        return vecops::buildVector(builder, elemTy, lanes, elems);
    }

    // The `<N x T>` slot type of a vector local named `nm`, or nullptr if `nm`
    // isn't a bound local of vector type.
    llvm::FixedVectorType* vectorSlotType(const std::string& nm) {
        auto it = slotTypes.find(nm);
        if (it == slotTypes.end() || !it->second->isVectorTy()) return nullptr;
        return llvm::cast<llvm::FixedVectorType>(it->second);
    }

    // `v.x` / `v.r` -> extractelement, or nullptr when `dot`'s base isn't a
    // vector local. Throws on a component letter beyond the lane count.
    llvm::Value* vectorComponentRead(const std::shared_ptr<DotExpression>& dot) {
        if (dot->getChildren().empty()) return nullptr;
        auto baseId = std::dynamic_pointer_cast<IdentifierExpression>(
            std::dynamic_pointer_cast<Expression>(dot->getChildren()[0]));
        if (!baseId) return nullptr;
        llvm::FixedVectorType* vt = vectorSlotType(baseId->getTextValue());
        if (!vt) return nullptr;
        int lane = vecops::laneForComponentName(dot->getIdentifier());
        llvm::Value* vec = builder.CreateLoad(
            vt, values[baseId->getTextValue()], baseId->getTextValue());
        if (lane >= 0) {
            if ((unsigned) lane >= vt->getNumElements())
                unsupported("vector component '." + dot->getIdentifier() +
                            "' is out of range");
            return vecops::extractLane(builder, vec, (unsigned) lane);
        }
        // Multi-component swizzle read `.xy`/`.xyz`/`.xxyy` -> `<M x T>`.
        auto lanes = vecops::swizzleLanes(dot->getIdentifier());
        if (lanes.empty())
            unsupported("vector component/swizzle '." + dot->getIdentifier() +
                        "' is not valid");
        for (int l : lanes)
            if ((unsigned) l >= vt->getNumElements())
                unsupported("swizzle '." + dot->getIdentifier() +
                            "' references a lane out of range");
        return vecops::swizzle(builder, vec, lanes);
    }

    // `v[i]` -> extractelement, or nullptr when the base isn't a vector local.
    llvm::Value* vectorIndexRead(const std::shared_ptr<ArrayIndexExpression>& ai) {
        auto baseId = std::dynamic_pointer_cast<IdentifierExpression>(
            exprChild(ai, 0));
        if (!baseId) return nullptr;
        llvm::FixedVectorType* vt = vectorSlotType(baseId->getTextValue());
        if (!vt) return nullptr;
        llvm::Value* vec = builder.CreateLoad(
            vt, values[baseId->getTextValue()], baseId->getTextValue());
        llvm::Value* idx = lowerExpr(exprChild(ai, 1));
        return vecops::extractLane(builder, vec, idx);
    }

    // `v.x = e` / `v[i] = e` (and the compound `op=` forms): load the slot,
    // insertelement the new lane value, store back. Returns false when `lhs`
    // isn't a vector-local component/index — the caller then takes the normal
    // (scalar / buffer) assignment path.
    bool tryVectorElementAssign(const std::shared_ptr<BinaryOpExpression>& bin,
                                const ExpressionPtr& lhs,
                                const ExpressionPtr& rhs) {
        std::string baseName;
        llvm::Value* laneIdx = nullptr;
        if (auto dot = std::dynamic_pointer_cast<DotExpression>(lhs)) {
            if (dot->getChildren().empty()) return false;
            auto baseId = std::dynamic_pointer_cast<IdentifierExpression>(
                std::dynamic_pointer_cast<Expression>(dot->getChildren()[0]));
            if (!baseId) return false;
            llvm::FixedVectorType* vt = vectorSlotType(baseId->getTextValue());
            if (!vt) return false;
            int lane = vecops::laneForComponentName(dot->getIdentifier());
            if (lane < 0 || (unsigned) lane >= vt->getNumElements()) {
                unsupported("vector component '." + dot->getIdentifier() +
                            "' is out of range");
            }
            baseName = baseId->getTextValue();
            laneIdx = builder.getInt32((unsigned) lane);
        } else if (auto ai = std::dynamic_pointer_cast<ArrayIndexExpression>(lhs)) {
            auto baseId = std::dynamic_pointer_cast<IdentifierExpression>(
                exprChild(ai, 0));
            if (!baseId || !vectorSlotType(baseId->getTextValue())) return false;
            baseName = baseId->getTextValue();
            laneIdx = lowerExpr(exprChild(ai, 1));
        } else {
            return false;
        }
        llvm::FixedVectorType* vt = vectorSlotType(baseName);
        llvm::Type* elemTy = vt->getElementType();
        llvm::Value* slot = values[baseName];
        llvm::Value* loaded = builder.CreateLoad(vt, slot, baseName);
        llvm::Value* rv = lowerExpr(rhs);
        BinaryOp op = bin->getBinaryOp();
        if (op != BINARY_OP_ASSIGN) {
            llvm::Value* cur = vecops::extractLane(builder, loaded, laneIdx);
            auto sit = signedness.find(baseName);
            rv = applyBinOp(compoundBase(op), cur, rv,
                            sit != signedness.end() ? sit->second : true,
                            elemTy->isFloatingPointTy());
        }
        rv = coerceTo(rv, elemTy);
        builder.CreateStore(vecops::insertLane(builder, loaded, rv, laneIdx),
                            slot);
        return true;
    }

    // `a.dot(b)`, `v.length()`, `v.normalize()` on a vector local `recv`.
    llvm::Value* lowerVectorMethod(
            const std::string& recv, const std::string& name,
            const std::shared_ptr<MethodCallExpression>& mc) {
        llvm::FixedVectorType* vt = vectorSlotType(recv);
        bool isFloat = vt->getElementType()->isFloatingPointTy();
        llvm::Value* self = builder.CreateLoad(vt, values[recv], recv);
        const auto& args = mc->getParameters();
        if (name == "dot") {
            if (isFloat) {
                if (args.size() != 1)
                    unsupported("Vector.dot expects one argument");
                return vecops::dot(builder, self,
                                   lowerExpr(args[0].expression), true);
            }
            // Integer dot (DP4a): Vector<int8,4>/<uint8,4> -> int32, with an
            // optional int32 accumulator. a.dot(b) = sum(a_i*b_i);
            // a.dot(b, acc) = acc + sum(a_i*b_i).
            unsigned lanes = vt->getNumElements();
            unsigned bits = vt->getElementType()->getIntegerBitWidth();
            if (lanes != 4 || bits != 8)
                unsupported("integer Vector.dot currently supports 4-lane "
                            "8-bit vectors (DP4a): Vector<int8,4> / "
                            "Vector<uint8,4>");
            if (args.size() != 1 && args.size() != 2)
                unsupported("Vector.dot expects (other) or (other, acc)");
            llvm::Value* other = lowerExpr(args[0].expression);
            llvm::Type* i32 = llvm::Type::getInt32Ty(builder.getContext());
            llvm::Value* acc = args.size() == 2
                ? coerceTo(lowerExpr(args[1].expression), i32)
                : llvm::ConstantInt::get(i32, 0);
            bool sgn = signedness.count(recv) ? signedness[recv] : true;
            return target.integerDot4x8(builder, mod, self, other, acc, sgn);
        }
        if (name == "length") {
            if (!isFloat) unsupported("Vector.length requires a "
                                      "floating-point element type");
            return vecops::length(builder, self);
        }
        if (name == "normalize") {
            if (!isFloat) unsupported("Vector.normalize requires a "
                                      "floating-point element type");
            return vecops::normalize(builder, self);
        }
        // B1 intrinsics A1 — element-wise min/max/clamp/lerp (float-only v1).
        // Scalar args (clamp bounds, lerp t) are coerced to the element type.
        llvm::Type* elemTy = vt->getElementType();
        if (name == "min" || name == "max") {
            if (!isFloat) unsupported("Vector." + name + " requires a "
                                      "floating-point element type");
            if (args.size() != 1) unsupported("Vector." + name + " expects one argument");
            llvm::Value* other = lowerExpr(args[0].expression);
            return name == "min"
                ? vecops::vmin(builder, self, other, isFloat, /*isSigned=*/true)
                : vecops::vmax(builder, self, other, isFloat, /*isSigned=*/true);
        }
        if (name == "clamp") {
            if (!isFloat) unsupported("Vector.clamp requires a "
                                      "floating-point element type");
            if (args.size() != 2) unsupported("Vector.clamp expects two arguments (lo, hi)");
            llvm::Value* lo = vecops::coerceScalar(builder, lowerExpr(args[0].expression), elemTy);
            llvm::Value* hi = vecops::coerceScalar(builder, lowerExpr(args[1].expression), elemTy);
            return vecops::clamp(builder, self, lo, hi, isFloat, /*isSigned=*/true);
        }
        if (name == "lerp") {
            if (!isFloat) unsupported("Vector.lerp requires a "
                                      "floating-point element type");
            if (args.size() != 2) unsupported("Vector.lerp expects two arguments (other, t)");
            llvm::Value* other = lowerExpr(args[0].expression);
            llvm::Value* t = vecops::coerceScalar(builder, lowerExpr(args[1].expression), elemTy);
            return vecops::lerp(builder, self, other, t);
        }
        // B1 intrinsics A2 — cross (3-D) / reflect / refract / distance, float-only.
        if (name == "cross" || name == "reflect" || name == "refract"
                || name == "distance") {
            if (!isFloat) unsupported("Vector." + name + " requires a "
                                      "floating-point element type");
            if (name == "cross" && vt->getNumElements() != 3)
                unsupported("Vector.cross requires 3-component vectors");
            unsigned want = name == "refract" ? 2u : 1u;
            if (args.size() != want)
                unsupported("Vector." + name + " argument count");
            llvm::Value* other = lowerExpr(args[0].expression);
            if (name == "cross")    return vecops::cross(builder, self, other);
            if (name == "reflect")  return vecops::reflect(builder, self, other);
            if (name == "distance") return vecops::distance(builder, self, other);
            llvm::Value* eta = vecops::coerceScalar(builder, lowerExpr(args[1].expression), elemTy);
            return vecops::refract(builder, self, other, eta);
        }
        unsupported("unknown Vector method '" + name + "'");
    }

    // `m.transpose()`, `m.identity()`, `m.row(r)`, `m.col(c)`, `m.hadamard(b)`
    // on a matrix local `recv` (B1). Mirrors the host MethodCallExpression
    // interception via the shared `matops` helpers — the result's shape is
    // carried by the assignment target's declared type (matrixShapes for a
    // Matrix result, vectorSlotType for a row/col Vector), so this only has to
    // produce the right flat `<R*C x T>` / `<C x T>` / `<R x T>` value. Must be
    // dispatched BEFORE lowerVectorMethod: a matrix slot is itself a vector
    // type, so vectorSlotType(recv) is non-null for a matrix local too.
    llvm::Value* lowerMatrixMethod(
            const std::string& recv, const std::string& name,
            const std::shared_ptr<MethodCallExpression>& mc) {
        auto sh = matrixShapes.find(recv);
        unsigned R = sh->second.first, C = sh->second.second;
        llvm::FixedVectorType* mt = vectorSlotType(recv);
        llvm::Type* elemTy = mt->getElementType();
        bool isFloat = elemTy->isFloatingPointTy();
        llvm::Value* self = builder.CreateLoad(mt, values[recv], recv);
        const auto& args = mc->getParameters();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        if (name == "transpose") {
            if (!args.empty())
                unsupported("Matrix.transpose takes no arguments");
            return matops::transpose(builder, self, R, C);
        }
        if (name == "identity") {
            if (!args.empty())
                unsupported("Matrix.identity takes no arguments");
            if (R != C)
                unsupported("Matrix.identity requires a square matrix");
            return matops::identity(builder, elemTy, R);
        }
        if (name == "row" || name == "col") {
            if (args.size() != 1)
                unsupported("Matrix." + name + " expects one argument");
            llvm::Value* idx = coerceTo(lowerExpr(args[0].expression), i32);
            return name == "row" ? matops::row(builder, self, R, C, idx)
                                 : matops::col(builder, self, R, C, idx);
        }
        if (name == "hadamard") {
            if (args.size() != 1)
                unsupported("Matrix.hadamard expects one argument");
            llvm::Value* other = lowerExpr(args[0].expression);
            return matops::hadamard(builder, self, other, isFloat);
        }
        // determinant() -> scalar, inverse() -> Matrix<T,N,N>. Square n in
        // {2,3,4}, float element only.
        if (name == "determinant" || name == "inverse") {
            if (R != C || R < 2 || R > 4)
                unsupported("Matrix." + name + " requires a square 2x2/3x3/4x4 matrix");
            if (!isFloat)
                unsupported("Matrix." + name + " requires a floating-point element type");
            if (!args.empty())
                unsupported("Matrix." + name + " takes no arguments");
            return name == "determinant" ? matops::determinant(builder, self, R)
                                         : matops::inverse(builder, self, R);
        }
        unsupported("unknown Matrix method '" + name + "'");
    }

    // Quaternion methods on a quaternion local `recv`: normalize/conjugate/
    // length/dot/nlerp. (slerp is a device-transcendental follow-on.)
    llvm::Value* lowerQuaternionMethod(
            const std::string& recv, const std::string& name,
            const std::shared_ptr<MethodCallExpression>& mc) {
        llvm::FixedVectorType* vt = vectorSlotType(recv);
        llvm::Value* self = builder.CreateLoad(vt, values[recv], recv);
        const auto& args = mc->getParameters();
        if (name == "normalize") return vecops::normalize(builder, self);
        if (name == "conjugate") return quatops::conjugate(builder, self);
        if (name == "length")    return vecops::length(builder, self);
        if (name == "dot") {
            if (args.size() != 1) unsupported("Quaternion.dot expects one argument");
            return vecops::dot(builder, self, lowerExpr(args[0].expression),
                               /*isFloat=*/true);
        }
        if (name == "nlerp") {
            if (args.size() != 2)
                unsupported("Quaternion.nlerp expects two arguments (other, t)");
            llvm::Value* other = lowerExpr(args[0].expression);
            llvm::Value* t = vecops::coerceScalar(
                builder, lowerExpr(args[1].expression), vt->getElementType());
            return quatops::nlerp(builder, self, other, t);
        }
        if (name == "slerp") {
            if (args.size() != 2)
                unsupported("Quaternion.slerp expects two arguments (other, t)");
            llvm::Value* other = lowerExpr(args[0].expression);
            llvm::Value* t = vecops::coerceScalar(
                builder, lowerExpr(args[1].expression), vt->getElementType());
            // Device: sin/acos route through the transcendental seam (AMD ocml).
            quatops::TrigEmitter trig =
                [&](const std::string& nm,
                    llvm::ArrayRef<llvm::Value*> as) -> llvm::Value* {
                    return target.transcendental(builder, mod, nm, as);
                };
            return quatops::slerp(builder, self, other, t, trig);
        }
        unsupported("unknown Quaternion method '" + name + "'");
    }

    // Decode `name.field` on a POD-struct kernel param to its field record, or
    // nullptr if `e` isn't that shape (a non-dot, a dot on a non-struct, or an
    // unknown field — the last surfaces as `unsupported` only at access time).
    const DeviceStructInfo::Field* structFieldOf(const ExpressionPtr& e) {
        auto dot = std::dynamic_pointer_cast<DotExpression>(e);
        if (!dot || dot->getChildren().empty()) return nullptr;
        auto baseId = std::dynamic_pointer_cast<IdentifierExpression>(
            std::dynamic_pointer_cast<Expression>(dot->getChildren()[0]));
        if (!baseId) return nullptr;
        auto sit = structFields.find(baseId->getTextValue());
        if (sit == structFields.end()) return nullptr;
        auto fit = sit->second.fields.find(dot->getIdentifier());
        if (fit == sit->second.fields.end()) return nullptr;
        return &fit->second;
    }

    // Read `name.field` on a POD-struct kernel param as an extractvalue from the
    // param's SSA aggregate (OpCompositeExtract on SPIR-V) — no pointer, so it's
    // valid in logical addressing. Returns nullptr when `e` isn't a struct-field
    // access; throws on a dot into a known struct param with an unknown field.
    llvm::Value* structFieldRead(const ExpressionPtr& e) {
        auto dot = std::dynamic_pointer_cast<DotExpression>(e);
        if (!dot || dot->getChildren().empty()) return nullptr;
        auto baseExpr = std::dynamic_pointer_cast<Expression>(dot->getChildren()[0]);
        // Single level: `param.field` — extractvalue at the field index. (For a
        // value-type field this returns the whole nested aggregate value.)
        if (auto baseId =
                std::dynamic_pointer_cast<IdentifierExpression>(baseExpr)) {
            auto vit = structValues.find(baseId->getTextValue());
            auto sit = structFields.find(baseId->getTextValue());
            if (vit == structValues.end() || sit == structFields.end())
                return nullptr;
            auto fit = sit->second.fields.find(dot->getIdentifier());
            if (fit == sit->second.fields.end())
                unsupported("unknown field '" + dot->getIdentifier() +
                            "' on struct param '" + baseId->getTextValue() + "'");
            return builder.CreateExtractValue(
                vit->second, {fit->second.index},
                baseId->getTextValue() + "." + dot->getIdentifier());
        }
        // Two level (S5): `param.vfield.subfield` on a nested @ValueType field —
        // a single multi-index extractvalue {vfield.index, subfield.index}.
        if (auto baseDot = std::dynamic_pointer_cast<DotExpression>(baseExpr)) {
            if (baseDot->getChildren().empty()) return nullptr;
            auto rootId = std::dynamic_pointer_cast<IdentifierExpression>(
                std::dynamic_pointer_cast<Expression>(baseDot->getChildren()[0]));
            if (!rootId) return nullptr;
            auto vit = structValues.find(rootId->getTextValue());
            auto sit = structFields.find(rootId->getTextValue());
            if (vit == structValues.end() || sit == structFields.end())
                return nullptr;
            auto vfit = sit->second.fields.find(baseDot->getIdentifier());
            if (vfit == sit->second.fields.end() || vfit->second.sub.empty())
                return nullptr;
            auto subit = vfit->second.sub.find(dot->getIdentifier());
            if (subit == vfit->second.sub.end())
                unsupported("unknown field '" + dot->getIdentifier() +
                            "' on value-type field '" + baseDot->getIdentifier() +
                            "' of struct param '" + rootId->getTextValue() + "'");
            return builder.CreateExtractValue(
                vit->second, {vfit->second.index, subit->second.index},
                rootId->getTextValue() + "." + baseDot->getIdentifier() +
                    "." + dot->getIdentifier());
        }
        return nullptr;
    }

    // Address (and element type) of an l-value. Buffer/array indexing and scalar
    // locals are supported as assignment targets; POD-struct fields are read-only.
    std::pair<llvm::Value*, llvm::Type*> lowerLValueAddr(const ExpressionPtr& e) {
        // POD-struct field `name.field` — read-only input in v1, never a target.
        if (structFieldOf(e))
            unsupported("POD-struct kernel params are read-only "
                        "(no 'name.field = ...')");
        // Scalar local / param: assign straight to its alloca slot.
        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(e)) {
            const std::string& nm = id->getTextValue();
            auto it = values.find(nm);
            if (it == values.end())
                unsupported("assignment to unbound local '" + nm + "'");
            return {it->second, slotTypes[nm]};
        }
        auto ai = std::dynamic_pointer_cast<ArrayIndexExpression>(e);
        if (!ai) unsupported("l-value that isn't a buffer index or local");
        // Bindless `bufs[idx][i]`: the base of this index is itself an index on a
        // Buffer<T>[] param. Select descriptor `idx` (bufferArrayElement), then
        // address element `i` through the normal bufferElementPtr seam.
        if (auto baseAi = std::dynamic_pointer_cast<ArrayIndexExpression>(
                exprChild(ai, 0))) {
            if (auto arrId = std::dynamic_pointer_cast<IdentifierExpression>(
                    exprChild(baseAi, 0))) {
                auto bab = bufferArrayBindings.find(arrId->getTextValue());
                if (bab != bufferArrayBindings.end()) {
                    llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
                    llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
                    ExpressionPtr dIdxExpr = exprChild(baseAi, 1);
                    llvm::Value* dIdx = lowerExpr(dIdxExpr);
                    if (dIdx->getType() != i32)
                        dIdx = builder.CreateIntCast(dIdx, i32,
                                                     exprSigned(dIdxExpr));
                    llvm::Value* handle = target.bufferArrayElement(
                        builder, mod, fn, bab->second.binding,
                        bufferArrayBases[arrId->getTextValue()],
                        bab->second.elemTy, dIdx);
                    ExpressionPtr eIdxExpr = exprChild(ai, 1);
                    llvm::Value* eIdx = lowerExpr(eIdxExpr);
                    if (eIdx->getType() != i64)
                        eIdx = builder.CreateIntCast(eIdx, i64,
                                                     exprSigned(eIdxExpr));
                    llvm::Value* addr = target.bufferElementPtr(
                        builder, mod, handle, bab->second.elemTy, eIdx);
                    return {addr, bab->second.elemTy};
                }
            }
        }
        auto baseId = std::dynamic_pointer_cast<IdentifierExpression>(
            exprChild(ai, 0));
        if (!baseId) unsupported("buffer index on a non-identifier base");
        // A typed dynamic shared array (Vulkan): index it as an array
        // (gep arrTy, gv, {0, i}) so the OpTypeArray survives in the SPIR-V.
        if (auto as = arrayShared.find(baseId->getTextValue());
            as != arrayShared.end()) {
            llvm::Value* idx = lowerExpr(exprChild(ai, 1));
            llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
            if (idx->getType() != i64)
                idx = builder.CreateIntCast(idx, i64,
                                            exprSigned(exprChild(ai, 1)));
            llvm::Value* zero = llvm::ConstantInt::get(i64, 0);
            llvm::Value* addr = builder.CreateGEP(
                as->second.second, as->second.first, {zero, idx}, "dynsh.idx");
            return {addr, bufferElems[baseId->getTextValue()]};
        }
        auto bv = bufferBases.find(baseId->getTextValue());
        auto be = bufferElems.find(baseId->getTextValue());
        if (bv == bufferBases.end() || be == bufferElems.end()) {
            unsupported("index on non-buffer '" + baseId->getTextValue() + "'");
        }
        ExpressionPtr idxExpr = exprChild(ai, 1);
        llvm::Value* idx = lowerExpr(idxExpr);
        // GEP wants an i64 index; widen by the index's signedness.
        if (idx->getType() != llvm::Type::getInt64Ty(ctx)) {
            idx = builder.CreateIntCast(idx, llvm::Type::getInt64Ty(ctx),
                                        exprSigned(idxExpr));
        }
        // Element pointer is a backend decision: NVPTX/AMDGPU GEP the base
        // pointer; Vulkan routes descriptor-buffer handles through getpointer
        // (shared-mem globals still GEP). See LoweringTarget::bufferElementPtr.
        llvm::Value* addr =
            target.bufferElementPtr(builder, mod, bv->second, be->second, idx);
        return {addr, be->second};
    }

    // Device builtins (Thread / Workgroup coordinates, Barrier). The mapping
    // from builtin name to which coordinate is read is SHARED; only the leaf
    // intrinsic emission is per-backend (LoweringTarget) — that split is the
    // measured seam (cajeta-amd.md §2).
    llvm::Value* lowerBuiltinCall(const std::shared_ptr<MethodCallExpression>& mc) {
        std::string recv;
        if (!mc->getChildren().empty()) {
            if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(
                    mc->getChildren()[0])) {
                recv = id->getTextValue();
            }
        }
        const std::string& name = mc->getMethodCallName();

        // Inline comparison-mask methods: `(a OP b).all()/.any()/.select(x,y)`.
        // The receiver is a non-identifier expression yielding a `<N x i1>` mask
        // (vector/matrix comparisons lower to masks via applyBinOp). all/any
        // reduce to a boolean; select blends two values per lane.
        if (recv.empty() && (name == "all" || name == "any" || name == "select")
                && !mc->getChildren().empty()) {
            if (auto recvExpr = std::dynamic_pointer_cast<Expression>(
                    mc->getChildren()[0])) {
                llvm::Value* mask = lowerExpr(recvExpr);
                if (mask && mask->getType()->isVectorTy()
                        && mask->getType()->getScalarType()->isIntegerTy(1)) {
                    if (name == "all") return builder.CreateAndReduce(mask);
                    if (name == "any") return builder.CreateOrReduce(mask);
                    const auto& sargs = mc->getParameters();
                    if (sargs.size() != 2)
                        unsupported("mask select expects (whenTrue, whenFalse)");
                    llvm::Value* a = lowerExpr(sargs[0].expression);
                    llvm::Value* b = lowerExpr(sargs[1].expression);
                    return builder.CreateSelect(mask, a, b, "mask.select");
                }
            }
        }

        if (recv == "Thread") {
            if (name == "x") return target.threadId(builder, mod, 0);
            if (name == "y") return target.threadId(builder, mod, 1);
            if (name == "z") return target.threadId(builder, mod, 2);
            if (name == "globalIdX") return target.globalId(builder, mod, 0);
            if (name == "globalIdY") return target.globalId(builder, mod, 1);
            if (name == "globalIdZ") return target.globalId(builder, mod, 2);
            // Thread.clock() — a free-running hardware counter (uint64) for
            // in-kernel timing (SPV_KHR_shader_clock on Vulkan; native clock on
            // AMD/NVIDIA/CPU). Ticks are for relative measurement, not seconds.
            if (name == "clock") return target.readClock(builder, mod);
        } else if (recv == "Workgroup") {
            if (name == "x") return target.workgroupId(builder, mod, 0);
            if (name == "y") return target.workgroupId(builder, mod, 1);
            if (name == "z") return target.workgroupId(builder, mod, 2);
            if (name == "dimX") return target.workgroupDim(builder, mod, 0);
            if (name == "dimY") return target.workgroupDim(builder, mod, 1);
            if (name == "dimZ") return target.workgroupDim(builder, mod, 2);
        } else if (recv == "Barrier") {
            if (name == "workgroup") {
                target.workgroupBarrier(builder, mod);
                return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
            }
        } else if (recv == "Wave") {
            const auto& args = mc->getParameters();
            if (name == "width") return target.waveWidth(builder, mod);
            if (name == "laneId") return target.waveLaneId(builder, mod);
            if (name == "isFirstLane") {
                // A width-agnostic cooperation helper, built on laneId(): the
                // "one lane commits the wave's result" guard. Lowered here
                // (not a seam point) so every backend gets it for free.
                llvm::Value* lane = target.waveLaneId(builder, mod);
                return builder.CreateICmpEQ(
                    lane, llvm::ConstantInt::get(lane->getType(), 0),
                    "wave.isfirst");
            }
            // The cross-lane ops (shuffle/ballot/reduce) read other lanes' data,
            // so their result depends on which lanes are converged. Flag the
            // kernel so finalization can request maximal reconvergence (Vulkan
            // OpExecutionMode MaximallyReconvergesKHR) — the guarantee that
            // source-converged lanes stay converged. The pure per-lane queries
            // (width/laneId/isFirstLane) above don't need it.
            if (name == "shuffleSync") {
                if (args.size() != 2) unsupported("Wave.shuffleSync arity");
                usedSubgroupOp_ = true;
                llvm::Value* value = lowerExpr(args[0].expression);
                llvm::Value* srcLane = lowerExpr(args[1].expression);
                return target.waveShuffle(builder, mod, value, srcLane);
            }
            if (name == "ballotSync") {
                if (args.size() != 1) unsupported("Wave.ballotSync arity");
                usedSubgroupOp_ = true;
                llvm::Value* pred = toI1(lowerExpr(args[0].expression));
                return target.waveBallot(builder, mod, pred);
            }
            if (name == "reduceSum") {
                if (args.size() != 1) unsupported("Wave.reduceSum arity");
                usedSubgroupOp_ = true;
                return target.waveReduceSum(builder, mod,
                                            lowerExpr(args[0].expression));
            }
            if (name == "rotate") {
                if (args.size() != 2) unsupported("Wave.rotate arity");
                usedSubgroupOp_ = true;
                llvm::Value* value = lowerExpr(args[0].expression);
                llvm::Value* delta = lowerExpr(args[1].expression);
                return target.waveRotate(builder, mod, value, delta);
            }
            {
                // The reduction family beyond sum (Wave.reduce{Max,Min,And,Or,
                // Xor}) — unsigned, uint32; native on every backend.
                using WROp = LoweringTarget::WaveReduceOp;
                const WROp* op = nullptr;
                static const WROp kMax = WROp::Max, kMin = WROp::Min,
                                  kAnd = WROp::And, kOr = WROp::Or,
                                  kXor = WROp::Xor;
                if (name == "reduceMax") op = &kMax;
                else if (name == "reduceMin") op = &kMin;
                else if (name == "reduceAnd") op = &kAnd;
                else if (name == "reduceOr") op = &kOr;
                else if (name == "reduceXor") op = &kXor;
                if (op) {
                    if (args.size() != 1) unsupported("Wave.reduce arity");
                    usedSubgroupOp_ = true;
                    return target.waveReduce(builder, mod, *op,
                                             lowerExpr(args[0].expression));
                }
            }
            if (name == "prefixSum" || name == "prefixProduct") {
                if (args.size() != 1) unsupported("Wave.prefix arity");
                usedSubgroupOp_ = true;
                auto sop = name == "prefixSum"
                    ? LoweringTarget::WaveScanOp::Sum
                    : LoweringTarget::WaveScanOp::Product;
                return target.waveScan(builder, mod, sop,
                                       lowerExpr(args[0].expression));
            }
        } else if (recv == "Quad") {
            // Quad (2x2) cross-lane ops — broadcast/swap/all/any. Cross-lane like
            // the Wave ops, so flag the kernel for maximal reconvergence.
            const auto& args = mc->getParameters();
            if (name == "broadcast") {
                if (args.size() != 2) unsupported("Quad.broadcast arity");
                usedSubgroupOp_ = true;
                llvm::Value* value = lowerExpr(args[0].expression);
                llvm::Value* index = lowerExpr(args[1].expression);
                return target.quadBroadcast(builder, mod, value, index);
            }
            if (name == "swapHorizontal" || name == "swapVertical" ||
                name == "swapDiagonal") {
                if (args.size() != 1) unsupported("Quad.swap arity");
                usedSubgroupOp_ = true;
                unsigned dir = name == "swapHorizontal" ? 0
                             : name == "swapVertical"   ? 1 : 2;
                llvm::Value* value = lowerExpr(args[0].expression);
                return target.quadSwap(builder, mod, value, dir);
            }
            if (name == "all" || name == "any") {
                if (args.size() != 1) unsupported("Quad.vote arity");
                usedSubgroupOp_ = true;
                llvm::Value* pred = toI1(lowerExpr(args[0].expression));
                return name == "all" ? target.quadAll(builder, mod, pred)
                                     : target.quadAny(builder, mod, pred);
            }
        } else if (recv == "Bits") {
            // Per-invocation bit manipulation. No seam: these lower to
            // *generic* LLVM intrinsics that every backend (incl. the
            // Vulkan/Shader flavor) already selects to a single hardware
            // bit op — so they are handled here, once, for all targets.
            const auto& args = mc->getParameters();
            auto* i32 = llvm::Type::getInt32Ty(ctx);
            auto u32arg = [&](int i) {
                return builder.CreateZExtOrTrunc(
                    lowerExpr(args[i].expression), i32);
            };
            if (name == "reverse") {
                if (args.size() != 1) unsupported("Bits.reverse arity");
                return builder.CreateUnaryIntrinsic(
                    llvm::Intrinsic::bitreverse, u32arg(0));
            }
            if (name == "count") {
                if (args.size() != 1) unsupported("Bits.count arity");
                return builder.CreateUnaryIntrinsic(
                    llvm::Intrinsic::ctpop, u32arg(0));
            }
            if (name == "rotateLeft" || name == "rotateRight") {
                if (args.size() != 2) unsupported("Bits.rotate arity");
                llvm::Value* v = u32arg(0);
                llvm::Value* amt = u32arg(1);
                // Inline rotate: (v << s) | (v >> (32 - s)), s masked to
                // [0,31]. Deliberately NOT llvm.fshl/fshr — the SPIR-V
                // backend lowers those via a *generated helper function*
                // (spirv.llvm_fsh?_i32) with external linkage, which pulls in
                // OpCapability Linkage and is rejected under Vulkan. The
                // shift/or expansion stays inline → core ops, spirv-val-clean.
                auto* w = builder.getInt32(32);
                auto* mask = builder.getInt32(31);
                llvm::Value* s = builder.CreateAnd(amt, mask, "bits.rot.s");
                // (32 - s) & 31 keeps the complementary shift in [0,31] so the
                // s==0 case is a 0-shift (no undef shift-by-32) and rotate is
                // identity.
                llvm::Value* cs = builder.CreateAnd(
                    builder.CreateSub(w, s), mask, "bits.rot.cs");
                bool left = (name == "rotateLeft");
                llvm::Value* a = left ? builder.CreateShl(v, s)
                                      : builder.CreateLShr(v, s);
                llvm::Value* b = left ? builder.CreateLShr(v, cs)
                                      : builder.CreateShl(v, cs);
                return builder.CreateOr(a, b, "bits.rot");
            }
        } else if (recv == "Math") {
            // Math.<fn>(...) inside a kernel — fully handled (returns a value or
            // a clean diagnostic). Mirrors the host Math lowering.
            return lowerMathCall(name, mc);
        } else if (recv == "CoopStage") {
            // CoopStage.panel(...) — the cooperative global→LDS staging copy for
            // tiled GEMM (the consume side is CooperativeMatrix.load(Shared<T>)).
            return lowerCoopStage(name, mc);
        }
        // Buffer atomics on the element pointer (the same bufferElementPtr seam as
        // buf[i]); every form returns the OLD value.
        //   Buffer<float32>: atomic{Add,Min,Max}(index, value) — the parallel-
        //     reduction / histogram lever (SPV_EXT_shader_atomic_float on Vulkan).
        //   Buffer<int32|uint32>: atomic{Add,Sub,Min,Max,And,Or,Xor,Exchange}
        //     (index, value) and atomicCompareExchange(index, expected, desired) —
        //     core OpAtomicI*/CompareExchange; the universal concurrency primitive.
        if (!recv.empty() && bufferBases.count(recv) &&
            (name == "atomicAdd" || name == "atomicSub" || name == "atomicMin" ||
             name == "atomicMax" || name == "atomicAnd" || name == "atomicOr" ||
             name == "atomicXor" || name == "atomicExchange" ||
             name == "atomicCompareExchange")) {
            const auto& args = mc->getParameters();
            const bool isCas = (name == "atomicCompareExchange");
            const size_t wantArgs = isCas ? 3 : 2;
            if (args.size() != wantArgs)
                unsupported("Buffer." + name +
                            (isCas ? " expects (index, expected, desired)"
                                   : " expects (index, value)"));
            llvm::Type* elemTy = bufferElems[recv];
            const bool elemSigned =
                bufferElemSigned.count(recv) ? bufferElemSigned[recv] : true;
            const bool elemIsFloat = elemTy->isFloatTy();
            const bool elemIsI32 = elemTy->isIntegerTy(32);
            llvm::Value* idx = lowerExpr(args[0].expression);
            llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
            if (idx->getType() != i64)
                idx = builder.CreateIntCast(idx, i64,
                                            exprSigned(args[0].expression));
            llvm::Value* ptr = target.bufferElementPtr(builder, mod,
                                                       bufferBases[recv],
                                                       elemTy, idx);
            if (isCas) {
                if (!elemIsI32)
                    unsupported("Buffer.atomicCompareExchange requires an "
                                "int32/uint32 buffer (v1)");
                llvm::Value* expected =
                    coerceTo(lowerExpr(args[1].expression), elemTy);
                llvm::Value* desired =
                    coerceTo(lowerExpr(args[2].expression), elemTy);
                return target.atomicCompareExchange(builder, mod, ptr, expected,
                                                    desired);
            }
            llvm::Value* val = coerceTo(lowerExpr(args[1].expression), elemTy);
            if (elemIsFloat) {
                if (name == "atomicAdd" || name == "atomicMin" ||
                    name == "atomicMax") {
                    LoweringTarget::AtomicFloatOp op =
                        name == "atomicAdd" ? LoweringTarget::AtomicFloatOp::Add
                      : name == "atomicMin" ? LoweringTarget::AtomicFloatOp::Min
                                            : LoweringTarget::AtomicFloatOp::Max;
                    return target.atomicFloatRMW(builder, mod, op, ptr, val);
                }
                unsupported("Buffer." + name + " is not supported on float32 "
                            "buffers (float atomics: atomicAdd/atomicMin/"
                            "atomicMax only)");
            }
            if (elemIsI32) {
                LoweringTarget::AtomicIntOp op =
                    name == "atomicAdd"  ? LoweringTarget::AtomicIntOp::Add
                  : name == "atomicSub"  ? LoweringTarget::AtomicIntOp::Sub
                  : name == "atomicMin"  ? LoweringTarget::AtomicIntOp::Min
                  : name == "atomicMax"  ? LoweringTarget::AtomicIntOp::Max
                  : name == "atomicAnd"  ? LoweringTarget::AtomicIntOp::And
                  : name == "atomicOr"   ? LoweringTarget::AtomicIntOp::Or
                  : name == "atomicXor"  ? LoweringTarget::AtomicIntOp::Xor
                                         : LoweringTarget::AtomicIntOp::Exchange;
                return target.atomicIntRMW(builder, mod, op, ptr, val,
                                           elemSigned);
            }
            unsupported("Buffer." + name + " requires a float32 or int32/uint32 "
                        "buffer (v1)");
        }
        // Matrix<T,R,C> instance methods (B1): transpose/identity/row/col/
        // hadamard. Checked BEFORE the vector branch — a matrix local's slot is
        // a `<R*C x T>` vector type, so vectorSlotType(recv) is non-null for it.
        if (!recv.empty() && matrixShapes.count(recv)) {
            return lowerMatrixMethod(recv, name, mc);
        }
        // Quaternion methods: normalize/conjugate/length/dot/nlerp. Checked
        // BEFORE the vector branch — a quaternion local's slot is `<4 x T>`.
        if (!recv.empty() && quaternionLocals.count(recv)) {
            return lowerQuaternionMethod(recv, name, mc);
        }
        // Vector<T,N> instance methods: a.dot(b), v.length(), v.normalize().
        // `recv` names a vector local.
        if (!recv.empty() && vectorSlotType(recv)) {
            return lowerVectorMethod(recv, name, mc);
        }
        // RayQuery ops (Part C). `recv` names a RayQuery body local; the op
        // lowers to a backend ray-query intrinsic (Vulkan llvm.spv.ray.query.*).
        if (auto rq = rayQuerySlots.find(recv); rq != rayQuerySlots.end()) {
            return lowerRayQueryMethod(rq->second, name, mc);
        }
        // CooperativeMatrix ops (CM4). `recv` names a CooperativeMatrix body
        // local; load/splat/mma/store lower to the backend cooperative-matrix
        // seams (Vulkan llvm.spv.cooperative.matrix.*).
        if (auto cm = coopMatrixSlots.find(recv); cm != coopMatrixSlots.end()) {
            return lowerCoopMatrixMethod(recv, name, mc);
        }
        // Texture{2D,3D}.sample(sampler, u, v[, w]) (Item 8). `recv` names a
        // texture kernel param; the first arg is a sampler kernel param, then the
        // normalized coords (2 for a 2-D texture, 3 for a 3-D volume). Lowered to
        // the backend image-sample seam (sampleTexture / sampleTexture3D).
        if (name == "sample" || name == "sampleLod") {
            auto th = textureHandles.find(recv);
            if (th != textureHandles.end()) {
                bool isLod = (name == "sampleLod");
                int dim = 2;
                if (auto d = textureDims.find(recv); d != textureDims.end())
                    dim = d->second;
                if (isLod && dim != 2)
                    unsupported("sampleLod is supported on Texture2D only");
                const auto& args = mc->getParameters();
                // Coord arity by texture kind: 1-D=1, 2-D=2, 3-D=3, 2-D array=3
                // (u,v,layer), cube=3 (x,y,z direction). sample takes a Sampler
                // first, so #args = arity + 1 (sampleLod adds the lod → arity + 2).
                int arity = textureCoordArity(dim);
                size_t expected = isLod ? (size_t)(arity + 2) : (size_t)(arity + 1);
                if ((size_t) args.size() != expected)
                    unsupported(isLod
                                ? "Texture2D.sampleLod expects (Sampler, u, v, lod)"
                                : (dim == 3
                                   ? "Texture3D.sample expects (Sampler, u, v, w)"
                                   : (dim == 1
                                      ? "Texture1D.sample expects (Sampler, u)"
                                      : (dim == 4
                                         ? "Texture2DArray.sample expects (Sampler, u, v, layer)"
                                         : (dim == 5
                                            ? "TextureCube.sample expects (Sampler, x, y, z)"
                                            : "Texture2D.sample expects (Sampler, u, v)")))));
                // Sampling is float-only: the hardware texture unit cannot filter
                // integer texels, so a Texture<int32>/<uint32> is fetch-only.
                if (auto tt = textureTexelTypes.find(recv);
                        tt != textureTexelTypes.end() &&
                        tt->second && tt->second->isIntegerTy()) {
                    throw cajeta::Exception(
                        "Texture<int>.sample is not supported — integer textures "
                        "cannot be filtered by the hardware sampler; use fetch(...) "
                        "for an exact integer-texel read", "XPU-N01");
                }
                llvm::Value* samp = resolveSamplerArg(args[0].expression);
                llvm::Value* u = toFloat(lowerExpr(args[1].expression));
                // 1-D: single coord, no v/w, no lod — dispatch before reading v.
                if (dim == 1)
                    return target.sampleTexture1D(builder, mod, th->second, samp, u);
                llvm::Value* v = toFloat(lowerExpr(args[2].expression));
                // 2-D array: (u, v, layer) — layer is an INTEGER array index (i32),
                // not a normalized coord; the seam converts it where the HW wants
                // a float layer coordinate.
                if (dim == 4) {
                    llvm::Value* layer = toI32(lowerExpr(args[3].expression));
                    return target.sampleTexture2DArray(builder, mod, th->second,
                                                       samp, u, v, layer);
                }
                // Cube: (x, y, z) direction vector — three float coords, like 3-D
                // but routed to the cube seam (the image is Dim=Cube, not 3-D).
                if (dim == 5) {
                    llvm::Value* z = toFloat(lowerExpr(args[3].expression));
                    return target.sampleTextureCube(builder, mod, th->second, samp,
                                                    u, v, z);
                }
                if (dim == 3) {
                    llvm::Value* w = toFloat(lowerExpr(args[3].expression));
                    return target.sampleTexture3D(builder, mod, th->second, samp,
                                                  u, v, w);
                }
                // 2-D: explicit mip level (0.0 for plain sample).
                llvm::Value* lod = isLod
                    ? toFloat(lowerExpr(args[3].expression))
                    : llvm::ConstantFP::get(llvm::Type::getFloatTy(mod.getContext()), 0.0);
                return target.sampleTexture(builder, mod, th->second, samp, u, v,
                                            lod);
            }
        }
        // Texture{2D,3D}.fetch(x, y[, z]) (texelFetch): unfiltered, sampler-free
        // read of the exact integer texel/voxel. `recv` names a texture kernel
        // param (the same sampled-image handle as sample); no Sampler arg. Lowered
        // to the backend unfiltered image-fetch seam (fetchTexture / fetchTexture3D).
        if (name == "fetch" || name == "fetchLod") {
            auto th = textureHandles.find(recv);
            if (th != textureHandles.end()) {
                bool isLod = (name == "fetchLod");
                int dim = 2;
                if (auto d = textureDims.find(recv); d != textureDims.end())
                    dim = d->second;
                if (isLod && dim != 2)
                    unsupported("fetchLod is supported on Texture2D only");
                // Cube textures have no integer texelFetch in v1 (a cube is read
                // by a direction vector through sample); reject it cleanly.
                if (dim == 5)
                    unsupported("TextureCube has no fetch — sample(s, x, y, z) "
                                "reads a cube by direction vector");
                const auto& args = mc->getParameters();
                // fetch coord arity by kind: 1-D=1, 2-D=2, 3-D=3, 2-D array=3
                // (x,y,layer). fetchLod (2-D only) adds the lod → arity + 1.
                int arity = textureCoordArity(dim);
                size_t expected = isLod ? (size_t)(arity + 1) : (size_t) arity;
                if ((size_t) args.size() != expected)
                    unsupported(isLod
                                ? "Texture2D.fetchLod expects (x, y, lod)"
                                : (dim == 3 ? "Texture3D.fetch expects (x, y, z)"
                                   : (dim == 1 ? "Texture1D.fetch expects (x)"
                                      : (dim == 4
                                         ? "Texture2DArray.fetch expects (x, y, layer)"
                                         : "Texture2D.fetch expects (x, y)"))));
                llvm::Value* x = toI32(lowerExpr(args[0].expression));
                // Texel scalar T (float by default; i32 for integer textures) —
                // the backend builds a <4 x T> result from it.
                llvm::Type* texelTy = llvm::Type::getFloatTy(mod.getContext());
                if (auto tt = textureTexelTypes.find(recv);
                        tt != textureTexelTypes.end() && tt->second)
                    texelTy = tt->second;
                // 1-D: single coord, no y/z, no lod — dispatch before reading y.
                if (dim == 1)
                    return target.fetchTexture1D(builder, mod, th->second, x, texelTy);
                llvm::Value* y = toI32(lowerExpr(args[1].expression));
                // 2-D array: (x, y, layer) — layer is the integer array index.
                if (dim == 4) {
                    llvm::Value* layer = toI32(lowerExpr(args[2].expression));
                    return target.fetchTexture2DArray(builder, mod, th->second, x, y,
                                                      layer, texelTy);
                }
                if (dim == 3) {
                    llvm::Value* z = toI32(lowerExpr(args[2].expression));
                    return target.fetchTexture3D(builder, mod, th->second, x, y, z,
                                                 texelTy);
                }
                // 2-D: explicit mip level (0 for plain fetch).
                llvm::Value* lod = isLod
                    ? toI32(lowerExpr(args[2].expression))
                    : llvm::ConstantInt::get(llvm::Type::getInt32Ty(mod.getContext()), 0);
                return target.fetchTexture(builder, mod, th->second, x, y,
                                           texelTy, lod);
            }
        }
        // Image2D.store(x, y, value) (writable images). `recv` names a storage-
        // image kernel param; (x, y) are INTEGER texel coords and value the f32
        // texel. Lowered to the backend image-write seam (Vulkan OpImageWrite).
        if (name == "store") {
            auto ih = imageHandles.find(recv);
            if (ih != imageHandles.end()) {
                const auto& args = mc->getParameters();
                if (args.size() != 3)
                    unsupported("Image2D.store expects (x, y, value)");
                llvm::Value* x = toI32(lowerExpr(args[0].expression));
                llvm::Value* y = toI32(lowerExpr(args[1].expression));
                llvm::Value* val = toFloat(lowerExpr(args[2].expression));
                target.storeImage(builder, mod, ih->second, x, y, val);
                // void op — return a dummy i32 0 (the sibling void device ops,
                // e.g. CooperativeMatrix.store, use the same placeholder).
                return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
            }
        }
        // Image2D.load(x, y) (writable images): read the texel at INTEGER coords
        // (x, y) → f32. The read twin of store; same STORAGE_IMAGE handle. Lowered
        // to the backend image-read seam (Vulkan OpImageRead).
        if (name == "load") {
            auto ih = imageHandles.find(recv);
            if (ih != imageHandles.end()) {
                const auto& args = mc->getParameters();
                if (args.size() != 2)
                    unsupported("Image2D.load expects (x, y)");
                llvm::Value* x = toI32(lowerExpr(args[0].expression));
                llvm::Value* y = toI32(lowerExpr(args[1].expression));
                return target.loadImage(builder, mod, ih->second, x, y);
            }
        }

        // A user-defined @Device helper call (resolved within the kernel's
        // class). Lower the helper to a device function (cached) and call it.
        if (auto m = resolveDeviceMethod(recv, name, mc)) {
            llvm::Function* hfn = lowerDeviceFn(m);
            const auto& args = mc->getParameters();
            std::vector<llvm::Value*> argv;
            argv.reserve(args.size());
            for (unsigned i = 0; i < args.size(); ++i)
                argv.push_back(coerceTo(lowerExpr(args[i].expression),
                                        hfn->getArg(i)->getType()));
            return builder.CreateCall(hfn, argv, hfn->getReturnType()->isVoidTy()
                                                     ? "" : "dev.call");
        }
        unsupported("device builtin '" + recv + "." + name + "()'");
    }

    // Resolve the sampler argument of a `tex.sample(sampler, ...)` to its
    // materialized descriptor. v1: the sampler must be a bare identifier naming
    // a Sampler kernel param (the descriptor model — a sampler isn't an
    // expressible value in a kernel body, only a bound resource).
    llvm::Value* resolveSamplerArg(const ExpressionPtr& e) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(e)) {
            auto it = samplerHandles.find(id->getTextValue());
            if (it != samplerHandles.end()) return it->second;
        }
        unsupported("Texture.sample: first argument must be a Sampler "
                    "kernel parameter");
    }

    // Resolve a core stdlib class by canonical name (parsed eagerly into the
    // process-global canonicalMap at compiler init), for the software ray-query
    // dispatch into cajeta.gpu.core.SoftwareRayQuery / SwRayCursor.
    std::shared_ptr<CajetaClass> coreClass(const std::string& canonical) {
        auto& cm = CajetaType::getCanonicalMap();
        auto it = cm.find(canonical);
        return it == cm.end()
            ? nullptr
            : std::dynamic_pointer_cast<CajetaClass>(it->second);
    }

    // A @Device method of a core class, by name (first match by name).
    MethodPtr coreDeviceMethod(const std::string& canonical,
                               const std::string& method) {
        auto c = coreClass(canonical);
        if (!c) return nullptr;
        for (auto& kv : c->getMethods())
            if (kv.second && kv.second->getName() == method) return kv.second;
        return nullptr;
    }

    // The SwRayCursor device struct + field map (cached). Empty if unavailable.
    const DeviceStructInfo& swCursorInfo() {
        if (!swCursorCached) {
            auto c = coreClass("cajeta.gpu.core.SwRayCursor");
            swCursorInfoCache = c ? deviceStructInfo(c, ctx) : DeviceStructInfo{};
            swCursorCached = true;
        }
        return swCursorInfoCache;
    }

    // Software-tier RayQuery op lowering (ray-query-to-core inc 1). The RayQuery
    // alloca holds a SwRayCursor; each op is a call into the portable
    // SoftwareRayQuery @Device walk with the cursor read/written back, plus field
    // reads off the cursor — no native ray-query seam (XPU-N02 is not thrown).
    llvm::Value* lowerSoftwareRayQueryMethod(
            llvm::Value* rqPtr, const std::string& name,
            const std::shared_ptr<MethodCallExpression>& mc) {
        const auto& args = mc->getParameters();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        const DeviceStructInfo& ci = swCursorInfo();
        if (!ci.type) unsupported("software RayQuery: SwRayCursor unavailable");
        llvm::Type* cursorTy = ci.type;
        auto fieldIdx = [&](const char* n) -> unsigned {
            auto it = ci.fields.find(n);
            if (it == ci.fields.end())
                unsupported(std::string("SwRayCursor missing field ") + n);
            return it->second.index;
        };

        if (name == "initialize") {
            if (args.size() != 11)
                unsupported("RayQuery.initialize expects (AccelerationStructure, "
                            "rayFlags, cullMask, originX, originY, originZ, tMin, "
                            "dirX, dirY, dirZ, tMax)");
            (void) f32;
            // Remember the AS (its handle = the software BVH buffer base) so each
            // proceed() can pass it to the walk; rayFlags/cullMask are v1-ignored.
            rayQueryBvh[rqPtr] = resolveAccelArg(args[0].expression);
            // Seed a fresh cursor by value: ray fields from the call, traversal
            // state zeroed. Built here (insertvalue) rather than via a cajeta
            // initialize — a no-Buffer @Device method would be host-compiled and
            // choke on value-type construction (Method.cpp only host-stubs
            // Buffer-taking device methods). Consumer arg order:
            // (AS, rayFlags, cullMask, ox, oy, oz, tMin, dx, dy, dz, tMax).
            llvm::Value* cur = llvm::UndefValue::get(cursorTy);
            auto setField = [&](const char* fld, llvm::Value* v) {
                const auto& f = ci.fields.at(fld);
                cur = builder.CreateInsertValue(cur, coerceTo(v, f.type), {f.index});
            };
            auto setExpr = [&](const char* fld, const ExpressionPtr& e) {
                setField(fld, lowerExpr(e));
            };
            auto zero = [&](const char* fld) {
                const auto& f = ci.fields.at(fld);
                cur = builder.CreateInsertValue(
                    cur, llvm::Constant::getNullValue(f.type), {f.index});
            };
            setExpr("ox", args[3].expression);
            setExpr("oy", args[4].expression);
            setExpr("oz", args[5].expression);
            setExpr("tMin", args[6].expression);
            setExpr("dx", args[7].expression);
            setExpr("dy", args[8].expression);
            setExpr("dz", args[9].expression);
            setExpr("tMax", args[10].expression);
            zero("nextNode"); zero("hasCandidate");
            zero("candPrim"); zero("candKind"); zero("committed");
            zero("candT"); zero("candU"); zero("candV"); zero("candFront");
            zero("committedT"); zero("committedPrim");
            zero("committedU"); zero("committedV"); zero("committedFront");
            builder.CreateStore(cur, rqPtr);
            return llvm::ConstantInt::get(i32, 0);   // void statement
        }
        if (name == "proceed") {
            if (!args.empty()) unsupported("RayQuery.proceed takes no arguments");
            MethodPtr m =
                coreDeviceMethod("cajeta.gpu.core.SoftwareRayQuery", "step");
            if (!m) unsupported("cajeta.gpu.core.SoftwareRayQuery.step unavailable");
            llvm::Function* hfn = lowerDeviceFn(m);
            auto bit = rayQueryBvh.find(rqPtr);
            if (bit == rayQueryBvh.end())
                unsupported("RayQuery.proceed() before initialize()");
            llvm::Value* h = bit->second;
            llvm::Type* bvhTy = hfn->getArg(1)->getType();
            llvm::Value* bvh = h->getType()->isPointerTy()
                ? builder.CreateBitCast(h, bvhTy)
                : builder.CreateIntToPtr(h, bvhTy, "rq.bvh");
            llvm::Value* cur = builder.CreateLoad(cursorTy, rqPtr, "rq.cur");
            llvm::Value* nc = builder.CreateCall(hfn, {cur, bvh}, "rq.step");
            builder.CreateStore(nc, rqPtr);
            llvm::Value* has =
                builder.CreateExtractValue(nc, {fieldIdx("hasCandidate")}, "rq.has");
            return builder.CreateICmpNE(
                has, llvm::ConstantInt::get(has->getType(), 0), "rq.proceed");
        }
        if (name == "committedType" || name == "candidateType") {
            if (!args.empty())
                unsupported("RayQuery." + name + " takes no arguments");
            llvm::Value* cur = builder.CreateLoad(cursorTy, rqPtr, "rq.cur");
            unsigned idx = fieldIdx(name == "committedType" ? "committed"
                                                            : "candKind");
            return builder.CreateExtractValue(cur, {idx}, "rq.type");
        }
        if (name == "candidatePrimitiveIndex") {
            if (!args.empty())
                unsupported("RayQuery.candidatePrimitiveIndex takes no arguments");
            llvm::Value* cur = builder.CreateLoad(cursorTy, rqPtr, "rq.cur");
            return builder.CreateExtractValue(cur, {fieldIdx("candPrim")},
                                              "rq.prim");
        }
        // Candidate geometry getters (inc 3): the Möller-Trumbore hit distance +
        // barycentrics, read off the cursor fields step() populated.
        if (name == "candidateDistance" || name == "candidateBarycentricU" ||
            name == "candidateBarycentricV") {
            if (!args.empty())
                unsupported("RayQuery." + name + " takes no arguments");
            const char* fld = name == "candidateDistance"     ? "candT"
                            : name == "candidateBarycentricU" ? "candU"
                                                              : "candV";
            llvm::Value* cur = builder.CreateLoad(cursorTy, rqPtr, "rq.cur");
            return builder.CreateExtractValue(cur, {fieldIdx(fld)}, "rq.cand");
        }
        // confirm/generate (inc 3b): copy the current candidate into the committed
        // slot and shrink tMax to the hit distance, so the rest of the walk only
        // finds closer hits (the last commit is the nearest). confirm = triangle
        // (committed type 1, t/bary from the candidate); generate = AABB (type 2,
        // t from the shader argument).
        if (name == "confirmIntersection" || name == "generateIntersection") {
            bool tri = (name == "confirmIntersection");
            if (tri && !args.empty())
                unsupported("RayQuery.confirmIntersection takes no arguments");
            if (!tri && args.size() != 1)
                unsupported("RayQuery.generateIntersection expects (t)");
            llvm::Value* cur = builder.CreateLoad(cursorTy, rqPtr, "rq.cur");
            auto get = [&](const char* f) {
                return builder.CreateExtractValue(cur, {fieldIdx(f)}, f);
            };
            auto set = [&](const char* f, llvm::Value* v) {
                cur = builder.CreateInsertValue(
                    cur, coerceTo(v, ci.fields.at(f).type), {fieldIdx(f)});
            };
            llvm::Value* hitT = tri ? get("candT")
                                    : coerceTo(lowerExpr(args[0].expression), f32);
            set("committed", llvm::ConstantInt::get(i32, tri ? 1 : 2));
            set("committedT", hitT);
            set("committedPrim", get("candPrim"));
            if (tri) {
                set("committedU", get("candU"));
                set("committedV", get("candV"));
                set("committedFront", get("candFront"));
            }
            set("tMax", hitT);   // shrink the ray so later candidates are culled
            builder.CreateStore(cur, rqPtr);
            return llvm::ConstantInt::get(i32, 0);   // void statement
        }
        // Committed (nearest-hit) getters (inc 3b).
        if (name == "committedDistance" || name == "committedBarycentricU" ||
            name == "committedBarycentricV" || name == "committedPrimitiveIndex") {
            if (!args.empty())
                unsupported("RayQuery." + name + " takes no arguments");
            const char* fld = name == "committedDistance"     ? "committedT"
                            : name == "committedBarycentricU" ? "committedU"
                            : name == "committedBarycentricV" ? "committedV"
                                                              : "committedPrim";
            llvm::Value* cur = builder.CreateLoad(cursorTy, rqPtr, "rq.cur");
            return builder.CreateExtractValue(cur, {fieldIdx(fld)}, "rq.committed");
        }
        // Front-face getters (inc 3b): the cursor stores 1/0; yield i1.
        if (name == "candidateFrontFace" || name == "committedFrontFace") {
            if (!args.empty())
                unsupported("RayQuery." + name + " takes no arguments");
            const char* fld = (name == "committedFrontFace") ? "committedFront"
                                                             : "candFront";
            llvm::Value* cur = builder.CreateLoad(cursorTy, rqPtr, "rq.cur");
            llvm::Value* f = builder.CreateExtractValue(cur, {fieldIdx(fld)}, "rq.front");
            return builder.CreateICmpNE(
                f, llvm::ConstantInt::get(f->getType(), 0), "rq.frontb");
        }
        unsupported("software RayQuery." + name + "()");
    }

    // RayQuery op dispatch (Part C). `rqPtr` is the RayQuery alloca; the call is
    // lowered to the backend ray-query seam (Vulkan llvm.spv.ray.query.*), or —
    // on a software-tier backend — to the portable SoftwareRayQuery walk.
    llvm::Value* lowerRayQueryMethod(
            llvm::Value* rqPtr, const std::string& name,
            const std::shared_ptr<MethodCallExpression>& mc) {
        if (target.softwareRayQuery())
            return lowerSoftwareRayQueryMethod(rqPtr, name, mc);
        const auto& args = mc->getParameters();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        if (name == "initialize") {
            // (AccelerationStructure, rayFlags, cullMask, ox, oy, oz, tMin,
            //  dx, dy, dz, tMax) — origin/direction are component-wise scalars
            // the lowerer assembles into <3 x float> vectors.
            if (args.size() != 11)
                unsupported("RayQuery.initialize expects (AccelerationStructure, "
                            "rayFlags, cullMask, originX, originY, originZ, tMin, "
                            "dirX, dirY, dirZ, tMax)");
            llvm::Value* as = resolveAccelArg(args[0].expression);
            llvm::Value* flags = coerceTo(lowerExpr(args[1].expression), i32);
            llvm::Value* mask  = coerceTo(lowerExpr(args[2].expression), i32);
            llvm::Value* origin = makeVec3(args[3].expression, args[4].expression,
                                           args[5].expression);
            llvm::Value* tMin = toFloat(lowerExpr(args[6].expression));
            llvm::Value* dir = makeVec3(args[7].expression, args[8].expression,
                                        args[9].expression);
            llvm::Value* tMax = toFloat(lowerExpr(args[10].expression));
            target.rayQueryInitialize(builder, mod, rqPtr, as, flags, mask,
                                      origin, tMin, dir, tMax);
            // Void op used as a statement; the discarded result is irrelevant.
            return llvm::ConstantInt::get(i32, 0);
        }
        if (name == "proceed") {
            if (!args.empty()) unsupported("RayQuery.proceed takes no arguments");
            return target.rayQueryProceed(builder, mod, rqPtr);
        }
        if (name == "committedType" || name == "candidateType") {
            if (!args.empty())
                unsupported("RayQuery." + name + " takes no arguments");
            // OpRayQueryGetIntersectionTypeKHR intersection selector:
            // 1 = committed, 0 = candidate.
            llvm::Value* which =
                llvm::ConstantInt::get(i32, name == "committedType" ? 1 : 0);
            return target.rayQueryIntersectionType(builder, mod, rqPtr, which);
        }
        if (name == "candidatePrimitiveIndex" ||
            name == "committedPrimitiveIndex") {
            if (!args.empty())
                unsupported("RayQuery." + name + " takes no arguments");
            llvm::Value* sel = llvm::ConstantInt::get(
                i32, name == "committedPrimitiveIndex" ? 1 : 0);
            return target.rayQueryIntersectionPrimitiveIndex(builder, mod, rqPtr, sel);
        }
        // Distance getters (inc 3b): candidate / committed selector → f32.
        if (name == "candidateDistance" || name == "committedDistance") {
            if (!args.empty())
                unsupported("RayQuery." + name + " takes no arguments");
            llvm::Value* sel = llvm::ConstantInt::get(
                i32, name == "committedDistance" ? 1 : 0);
            return target.rayQueryIntersectionT(builder, mod, rqPtr, sel);
        }
        // Barycentric getters (inc 3b): read the <2 x f32> and extract u / v.
        if (name == "candidateBarycentricU" || name == "candidateBarycentricV" ||
            name == "committedBarycentricU" || name == "committedBarycentricV") {
            if (!args.empty())
                unsupported("RayQuery." + name + " takes no arguments");
            bool committed = (name.rfind("committed", 0) == 0);
            bool wantV = (name.back() == 'V');
            llvm::Value* sel = llvm::ConstantInt::get(i32, committed ? 1 : 0);
            llvm::Value* bary =
                target.rayQueryIntersectionBarycentrics(builder, mod, rqPtr, sel);
            return builder.CreateExtractElement(bary, wantV ? 1u : 0u, "rq.bary");
        }
        // Front-face getters (inc 3b): candidate / committed selector → i1.
        if (name == "candidateFrontFace" || name == "committedFrontFace") {
            if (!args.empty())
                unsupported("RayQuery." + name + " takes no arguments");
            llvm::Value* sel = llvm::ConstantInt::get(
                i32, name == "committedFrontFace" ? 1 : 0);
            return target.rayQueryIntersectionFrontFace(builder, mod, rqPtr, sel);
        }
        if (name == "confirmIntersection") {
            if (!args.empty())
                unsupported("RayQuery.confirmIntersection takes no arguments");
            target.rayQueryConfirmIntersection(builder, mod, rqPtr);
            return llvm::ConstantInt::get(i32, 0);   // void statement
        }
        if (name == "generateIntersection") {
            if (args.size() != 1)
                unsupported("RayQuery.generateIntersection expects (t)");
            llvm::Value* tHit = toFloat(lowerExpr(args[0].expression));
            target.rayQueryGenerateIntersection(builder, mod, rqPtr, tHit);
            return llvm::ConstantInt::get(i32, 0);   // void statement
        }
        unsupported("RayQuery." + name + "()");
    }

    // Resolve a `rq.initialize(as, ...)` acceleration-structure argument to its
    // materialized descriptor. Like a sampler, the AS must be a bare identifier
    // naming an AccelerationStructure kernel param (it is a bound resource, not
    // an expressible value in a kernel body).
    llvm::Value* resolveAccelArg(const ExpressionPtr& e) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(e)) {
            auto it = accelHandles.find(id->getTextValue());
            if (it != accelHandles.end()) return it->second;
        }
        unsupported("RayQuery.initialize: first argument must be an "
                    "AccelerationStructure kernel parameter");
    }

    // Build the device cooperative-matrix type for a
    // `CooperativeMatrix<T, Rows, Cols, Use>` local from its declared type
    // arguments: arg0 = element type, args 1-3 = the Rows/Cols/Use integer
    // constants. Delegates the actual type to the backend seam (Vulkan emits
    // OpTypeCooperativeMatrixKHR at Subgroup scope).
    CoopMatrixSlot buildCoopMatrixSlot(const CajetaTypePtr& declType,
                                       const std::string& nm) {
        auto cls = std::dynamic_pointer_cast<CajetaClass>(declType);
        if (!cls || cls->getTypeArguments().size() != 4)
            unsupported("CooperativeMatrix requires <T, Rows, Cols, Use>");
        const auto& targs = cls->getTypeArguments();
        llvm::Type* elem = deviceScalarType(targs[0], ctx);
        if (!elem)
            unsupported("CooperativeMatrix element type must be a numeric primitive");
        auto rows = std::dynamic_pointer_cast<CajetaConstantType>(targs[1]);
        auto cols = std::dynamic_pointer_cast<CajetaConstantType>(targs[2]);
        auto use  = std::dynamic_pointer_cast<CajetaConstantType>(targs[3]);
        if (!rows || !cols || !use)
            unsupported("CooperativeMatrix Rows/Cols/Use must be integer constants");
        CoopMatrixSlot s;
        s.elemType = elem;
        s.rows = (uint32_t) rows->getValue();
        s.cols = (uint32_t) cols->getValue();
        s.use  = (uint32_t) use->getValue();
        // The per-backend base tier, then the explicit CAJETA_GPU_COOPMATRIX_IMPL
        // override layered on top (the degrade seam's override face) — so a forced
        // "software" runs the portable tile even on a native-capable backend.
        auto baseTier = target.coopMatrixTier(elem, s.rows, s.cols, s.use);
        auto tier = resolveImplTier("COOPMATRIX", baseTier);
        if (tier == LoweringTarget::ImplTier::Portable) {
            // Portable flat tile: a `[Rows*Cols x elem]` array in Function
            // storage. Dynamic-index GEP (gather/scatter/matmul) is well-formed
            // on every backend including SPIR-V logical addressing.
            s.software = true;
            s.matrixType = llvm::ArrayType::get(elem, (uint64_t) s.rows * s.cols);
            s.alloca = entryAlloca(s.matrixType, nm);
            // One appraisal per GEMM: note the A operand (Use 0) — every matrix
            // multiply has exactly one, so the B/accumulator tiles don't re-note.
            // `forced` is true when the backend HAD a native config but the env
            // override took the portable path (so the note stays honest).
            if (s.use == 0)
                noteSoftwareCoopMatrix(elem, s.rows, s.cols,
                                       baseTier == LoweringTarget::ImplTier::Native);
        } else {
            s.software = false;
            s.matrixType = target.coopMatrixType(mod, elem, s.rows, s.cols, s.use);
            s.alloca = entryAlloca(s.matrixType, nm);
            target.prepareNativeCoopMatrix(fn);   // e.g. AMD: mark the kernel wave32
        }
        return s;
    }

    // The cajeta dtype name for a device scalar type, for diagnostics.
    std::string deviceScalarName(llvm::Type* t) {
        if (t->isBFloatTy()) return "bfloat16";
        if (t->isHalfTy())   return "float16";
        if (t->isFloatTy())  return "float32";
        if (t->isDoubleTy()) return "float64";
        if (t->isIntegerTy()) return "int" + std::to_string(t->getIntegerBitWidth());
        return "T";
    }

    // Sticky, non-dissuading appraisal (CM6): a `note:` — a severity BELOW
    // `warning:` — that tells the author a CooperativeMatrix took the portable
    // software path on this backend, without framing it as something to avoid.
    // The wording is a capability statement (it runs correctly here and lights
    // up hardware matrix cores automatically where the device exposes the dtype
    // config), and it is forward-looking, not corrective. Emitted once per
    // distinct (dtype, shape, backend).
    void noteSoftwareCoopMatrix(llvm::Type* elem, uint32_t rows, uint32_t cols,
                                bool forced) {
        std::string dt = deviceScalarName(elem);
        std::string key = dt + ":" + std::to_string(rows) + "x" +
                          std::to_string(cols) + "@" + target.name();
        if (!notedCoopTiers.insert(key).second) return;
        if (forced) {
            // The backend HAS a native config; CAJETA_GPU_COOPMATRIX_IMPL=software
            // forced the portable degrade (the override face of the degrade seam —
            // validates the portable tier against the native one on real silicon).
            std::cerr << "note: [mma-tiering] CooperativeMatrix<" << dt << ","
                      << rows << "," << cols << "> runs on the portable software "
                         "tile-matmul on the " << target.name() << " backend "
                         "because CAJETA_GPU_COOPMATRIX_IMPL=software forced it "
                         "(the backend DOES expose a native cooperative-matrix "
                         "config for " << dt << "; this is the explicit degrade "
                         "override)." << std::endl;
            return;
        }
        std::cerr << "note: [mma-tiering] CooperativeMatrix<" << dt << ","
                  << rows << "," << cols << "> runs on the portable software "
                     "tile-matmul on the " << target.name() << " backend (it "
                     "exposes no native cooperative-matrix config for " << dt
                  << "). The result is identical; it automatically uses the "
                     "hardware matrix cores on backends that do expose the "
                     "config (e.g. bf16 WMMA on AMD)." << std::endl;
    }

    // for (i32 iv = 0; iv < count; ++iv) body(iv) — a counted loop over a
    // compile-time bound, used to gather/scatter/multiply a software coop tile
    // without unrolling Rows*Cols (or Rows*Cols*K) ops. body() is emitted with
    // the builder positioned in the loop body; on return the insert point is the
    // loop exit. Nesting is fine (each call manages its own blocks).
    void emitCountedLoop(uint32_t count,
                         const std::function<void(llvm::Value*)>& body) {
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        llvm::Value* iv = entryAlloca(i32, "cm.iv");
        builder.CreateStore(llvm::ConstantInt::get(i32, 0), iv);
        auto* head = llvm::BasicBlock::Create(ctx, "cm.head", fn);
        auto* bodyBB = llvm::BasicBlock::Create(ctx, "cm.body", fn);
        auto* exit = llvm::BasicBlock::Create(ctx, "cm.exit", fn);
        builder.CreateBr(head);
        builder.SetInsertPoint(head);
        llvm::Value* cur = builder.CreateLoad(i32, iv, "cm.i");
        builder.CreateCondBr(
            builder.CreateICmpULT(cur, llvm::ConstantInt::get(i32, count)),
            bodyBB, exit);
        builder.SetInsertPoint(bodyBB);
        body(cur);
        builder.CreateStore(
            builder.CreateAdd(cur, llvm::ConstantInt::get(i32, 1)), iv);
        builder.CreateBr(head);
        builder.SetInsertPoint(exit);
    }

    // &tile[linIdx] for a software coop slot (a `[N x elem]` array alloca).
    llvm::Value* coopElemPtr(const CoopMatrixSlot& s, llvm::Value* linIdx) {
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        return builder.CreateInBoundsGEP(
            s.matrixType, s.alloca,
            {llvm::ConstantInt::get(i32, 0), linIdx}, "cm.elt");
    }

    // Resolve a Buffer kernel-param identifier to its base + element type, so a
    // software tile can index it per element (target.bufferElementPtr). Mirrors
    // resolveBufferTileArg but returns the base (not a pre-offset pointer).
    bool resolveBufferBase(const ExpressionPtr& e, llvm::Value*& base,
                           llvm::Type*& elemTy) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(e)) {
            auto bb = bufferBases.find(id->getTextValue());
            if (bb != bufferBases.end()) {
                base = bb->second;
                auto be = bufferElems.find(id->getTextValue());
                elemTy = be != bufferElems.end() ? be->second : nullptr;
                return true;
            }
        }
        return false;
    }

    // CoopStage.panel(dst, src, rowBase, colBase, rows, cols, ld) — the
    // workgroup-cooperative global→LDS staging copy (Option B). Every thread of
    // the workgroup strides over the rows*cols panel and copies it from a
    // row-major source (leading dim `ld`) into the contiguous LDS tile `dst`
    // (packed row-major, stride = cols). Both `dst` (a Shared<T> local) and `src`
    // (a Buffer<T> param) resolve through the same buffer maps; LLVM tracks the
    // address space on each base pointer, so the copy is backend-agnostic. The
    // caller owns the surrounding Barrier.workgroup() calls.
    llvm::Value* lowerCoopStage(const std::string& name,
                                const std::shared_ptr<MethodCallExpression>& mc) {
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
        if (name != "panel")
            unsupported("CoopStage." + name + "()");
        const auto& args = mc->getParameters();
        if (args.size() != 7)
            unsupported("CoopStage.panel expects (Shared dst, Buffer src, "
                        "rowBase, colBase, rows, cols, ld)");
        llvm::Value* dstBase = nullptr; llvm::Type* dstElem = nullptr;
        llvm::Value* srcBase = nullptr; llvm::Type* srcElem = nullptr;
        if (!resolveBufferBase(args[0].expression, dstBase, dstElem))
            unsupported("CoopStage.panel: dst must be a Shared<T> kernel local");
        if (!resolveBufferBase(args[1].expression, srcBase, srcElem))
            unsupported("CoopStage.panel: src must be a Buffer<T> kernel parameter");
        llvm::Value* rowBase = coerceTo(lowerExpr(args[2].expression), i32);
        llvm::Value* colBase = coerceTo(lowerExpr(args[3].expression), i32);
        llvm::Value* rows    = coerceTo(lowerExpr(args[4].expression), i32);
        llvm::Value* cols    = coerceTo(lowerExpr(args[5].expression), i32);
        llvm::Value* ld      = coerceTo(lowerExpr(args[6].expression), i32);
        llvm::Value* total   = builder.CreateMul(rows, cols, "stage.total");
        // for (e = Thread.x(); e < rows*cols; e += Workgroup.dimX())
        //     dst[e] = src[(rowBase + e/cols)*ld + (colBase + e%cols)]
        llvm::Value* tid  = coerceTo(target.threadId(builder, mod, 0), i32);
        llvm::Value* nthr = coerceTo(target.workgroupDim(builder, mod, 0), i32);
        llvm::Value* iv = entryAlloca(i32, "stage.e");
        builder.CreateStore(tid, iv);
        auto* head = llvm::BasicBlock::Create(ctx, "stage.head", fn);
        auto* body = llvm::BasicBlock::Create(ctx, "stage.body", fn);
        auto* exit = llvm::BasicBlock::Create(ctx, "stage.exit", fn);
        builder.CreateBr(head);
        builder.SetInsertPoint(head);
        llvm::Value* e = builder.CreateLoad(i32, iv, "stage.e.cur");
        builder.CreateCondBr(builder.CreateICmpULT(e, total), body, exit);
        builder.SetInsertPoint(body);
        llvm::Value* r = builder.CreateUDiv(e, cols);
        llvm::Value* c = builder.CreateURem(e, cols);
        llvm::Value* gIdx = builder.CreateAdd(
            builder.CreateMul(builder.CreateAdd(rowBase, r), ld),
            builder.CreateAdd(colBase, c), "stage.gidx");
        llvm::Value* sPtr = target.bufferElementPtr(
            builder, mod, srcBase, srcElem, builder.CreateZExt(gIdx, i64));
        llvm::Value* val = builder.CreateLoad(srcElem, sPtr, "stage.ld");
        llvm::Value* dPtr = target.bufferElementPtr(
            builder, mod, dstBase, dstElem, builder.CreateZExt(e, i64));
        builder.CreateStore(coerceTo(val, dstElem), dPtr);
        builder.CreateStore(builder.CreateAdd(e, nthr), iv);
        builder.CreateBr(head);
        builder.SetInsertPoint(exit);
        return llvm::ConstantInt::get(i32, 0);
    }

    // Resolve a CooperativeMatrix.load/store Buffer argument (a bare identifier
    // naming a Buffer kernel param) to a device pointer at element `offset` —
    // the base the cooperative-matrix load/store reads/writes Rows*Cols elements
    // from. `offset` selects a sub-tile of a larger row-major matrix (a tiled
    // GEMM walks it over the M/N/K tiles); 0 is the whole-buffer base.
    llvm::Value* resolveBufferTileArg(const ExpressionPtr& e,
                                      llvm::Value* offset) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(e)) {
            auto bb = bufferBases.find(id->getTextValue());
            if (bb != bufferBases.end()) {
                auto be = bufferElems.find(id->getTextValue());
                llvm::Type* elemTy = be != bufferElems.end() ? be->second : nullptr;
                llvm::Value* idx = builder.CreateZExtOrTrunc(
                    offset, llvm::Type::getInt64Ty(ctx), "cm.off");
                return target.bufferElementPtr(builder, mod, bb->second, elemTy,
                                               idx);
            }
        }
        unsupported("CooperativeMatrix load/store: argument must be a Buffer "
                    "kernel parameter");
    }

    // Resolve a CooperativeMatrix.mma operand (a bare identifier naming a
    // CooperativeMatrix local) to its slot.
    CoopMatrixSlot resolveCoopMatrixArg(const ExpressionPtr& e) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(e)) {
            auto it = coopMatrixSlots.find(id->getTextValue());
            if (it != coopMatrixSlots.end()) return it->second;
        }
        unsupported("CooperativeMatrix.mma: operands must be CooperativeMatrix "
                    "kernel locals");
    }

    // CooperativeMatrix op dispatch (CM4). The slot alloca holds the opaque tile;
    // load/splat/mma write it, store/mma read it. Lowered to the backend
    // cooperative-matrix seams (Vulkan llvm.spv.cooperative.matrix.*).
    llvm::Value* lowerCoopMatrixMethod(
            const std::string& recv, const std::string& name,
            const std::shared_ptr<MethodCallExpression>& mc) {
        const auto& args = mc->getParameters();
        CoopMatrixSlot slot = coopMatrixSlots[recv];
        if (slot.software) return lowerCoopMatrixMethodSoftware(recv, name, mc);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        if (name == "load" || name == "store") {
            if (args.size() != 4)
                unsupported("CooperativeMatrix." + name +
                            " expects (Buffer, offset, layout, stride)");
            llvm::Value* offset = lowerExpr(args[1].expression);
            llvm::Value* ptr = resolveBufferTileArg(args[0].expression, offset);
            llvm::Value* layout = coerceTo(lowerExpr(args[2].expression), i32);
            llvm::Value* stride = coerceTo(lowerExpr(args[3].expression), i32);
            if (name == "load") {
                llvm::Value* v = target.coopMatrixLoad(
                    builder, mod, ptr, layout, stride, slot.matrixType,
                    slot.rows, slot.cols, slot.use);
                builder.CreateStore(v, slot.alloca);
            } else {
                llvm::Value* v = builder.CreateLoad(slot.matrixType, slot.alloca,
                                                    recv + ".val");
                target.coopMatrixStore(builder, mod, ptr, v, layout, stride,
                                       slot.rows, slot.cols, slot.use);
            }
            return llvm::ConstantInt::get(i32, 0);
        }
        if (name == "splat") {
            if (args.size() != 1)
                unsupported("CooperativeMatrix.splat expects (value)");
            llvm::Value* val = lowerExpr(args[0].expression);
            llvm::Value* v =
                target.coopMatrixSplat(builder, mod, val, slot.matrixType);
            builder.CreateStore(v, slot.alloca);
            return llvm::ConstantInt::get(i32, 0);
        }
        if (name == "mma") {
            if (args.size() != 2)
                unsupported("CooperativeMatrix.mma expects (a, b)");
            CoopMatrixSlot a = resolveCoopMatrixArg(args[0].expression);
            CoopMatrixSlot b = resolveCoopMatrixArg(args[1].expression);
            if (a.software || b.software)
                unsupported("CooperativeMatrix.mma: a native accumulator cannot "
                            "consume software-tier operands — give all three "
                            "tiles the same dtype tier");
            llvm::Value* aVal = builder.CreateLoad(a.matrixType, a.alloca, "cm.a");
            llvm::Value* bVal = builder.CreateLoad(b.matrixType, b.alloca, "cm.b");
            llvm::Value* cVal =
                builder.CreateLoad(slot.matrixType, slot.alloca, "cm.c");
            llvm::Value* v = target.coopMatrixMulAdd(builder, mod, aVal, bVal,
                                                     cVal, slot.matrixType);
            builder.CreateStore(v, slot.alloca);
            return llvm::ConstantInt::get(i32, 0);
        }
        unsupported("CooperativeMatrix." + name + "()");
    }

    // Software cooperative-matrix ops (CM6): the slot is a flat `[R*C x elem]`
    // tile. splat fills it, load/store gather/scatter it from a Buffer with the
    // requested row/col-major layout + stride, and mma runs an honest triple
    // loop `result = c + a·b`. Floating-point GEMMs accumulate in f32 (the
    // bf16/f16 storage-plus-f32-compute model) and narrow to the accumulator's
    // dtype on store; integer GEMMs accumulate in the accumulator's int type.
    // Correct on every backend; the matrix cores are used instead wherever the
    // backend reports the Native tier (see coopMatrixTier).
    llvm::Value* lowerCoopMatrixMethodSoftware(
            const std::string& recv, const std::string& name,
            const std::shared_ptr<MethodCallExpression>& mc) {
        const auto& args = mc->getParameters();
        CoopMatrixSlot slot = coopMatrixSlots[recv];
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
        llvm::Type* elem = slot.elemType;
        const uint32_t R = slot.rows, C = slot.cols;

        if (name == "splat") {
            if (args.size() != 1)
                unsupported("CooperativeMatrix.splat expects (value)");
            llvm::Value* val = coerceTo(lowerExpr(args[0].expression), elem);
            emitCountedLoop(R * C, [&](llvm::Value* lin) {
                builder.CreateStore(val, coopElemPtr(slot, lin));
            });
            return llvm::ConstantInt::get(i32, 0);
        }

        if (name == "load" || name == "store") {
            if (args.size() != 4)
                unsupported("CooperativeMatrix." + name +
                            " expects (Buffer, offset, layout, stride)");
            llvm::Value* base = nullptr; llvm::Type* bElem = nullptr;
            if (!resolveBufferBase(args[0].expression, base, bElem))
                unsupported("CooperativeMatrix." + name +
                            ": argument must be a Buffer kernel parameter");
            llvm::Value* offset = coerceTo(lowerExpr(args[1].expression), i32);
            llvm::Value* layout = coerceTo(lowerExpr(args[2].expression), i32);
            llvm::Value* stride = coerceTo(lowerExpr(args[3].expression), i32);
            bool isLoad = (name == "load");
            emitCountedLoop(R, [&](llvm::Value* r) {
                emitCountedLoop(C, [&](llvm::Value* c) {
                    // buffer index: row-major r*stride+c, column-major c*stride+r.
                    llvm::Value* rm =
                        builder.CreateAdd(builder.CreateMul(r, stride), c);
                    llvm::Value* cm =
                        builder.CreateAdd(builder.CreateMul(c, stride), r);
                    llvm::Value* sel = builder.CreateSelect(
                        builder.CreateICmpEQ(layout,
                                             llvm::ConstantInt::get(i32, 0)),
                        rm, cm);
                    llvm::Value* bidx =
                        builder.CreateZExt(builder.CreateAdd(offset, sel), i64);
                    llvm::Value* bptr =
                        target.bufferElementPtr(builder, mod, base, bElem, bidx);
                    // tile linear index r*C + c.
                    llvm::Value* lin = builder.CreateAdd(
                        builder.CreateMul(r, llvm::ConstantInt::get(i32, C)), c);
                    llvm::Value* tptr = coopElemPtr(slot, lin);
                    if (isLoad) {
                        llvm::Value* v = builder.CreateLoad(bElem, bptr, "cm.ld");
                        builder.CreateStore(coerceTo(v, elem), tptr);
                    } else {
                        llvm::Value* v = builder.CreateLoad(elem, tptr, "cm.st");
                        builder.CreateStore(coerceTo(v, bElem), bptr);
                    }
                });
            });
            return llvm::ConstantInt::get(i32, 0);
        }

        if (name == "mma") {
            if (args.size() != 2)
                unsupported("CooperativeMatrix.mma expects (a, b)");
            CoopMatrixSlot a = resolveCoopMatrixArg(args[0].expression);
            CoopMatrixSlot b = resolveCoopMatrixArg(args[1].expression);
            if (!a.software || !b.software)
                unsupported("CooperativeMatrix.mma: a software accumulator cannot "
                            "consume native-tier operands — give all three tiles "
                            "the same dtype tier");
            // a is M x K, b is K x N, c/result (this slot) is M x N.
            const uint32_t M = a.rows, K = a.cols, N = b.cols;
            if (b.rows != K || slot.rows != M || slot.cols != N)
                unsupported("CooperativeMatrix.mma: shape mismatch (A is MxK, "
                            "B is KxN, accumulator is MxN)");
            llvm::Type* acc = slot.elemType;
            bool fp = acc->isFloatingPointTy();
            // FP: accumulate in f32 then narrow to the accumulator dtype.
            llvm::Type* compTy = fp ? llvm::Type::getFloatTy(ctx) : acc;
            emitCountedLoop(M, [&](llvm::Value* m) {
                emitCountedLoop(N, [&](llvm::Value* n) {
                    llvm::Value* cLin = builder.CreateAdd(
                        builder.CreateMul(m, llvm::ConstantInt::get(i32, N)), n);
                    llvm::Value* sumPtr = entryAlloca(compTy, "cm.sum");
                    builder.CreateStore(
                        coerceTo(builder.CreateLoad(acc, coopElemPtr(slot, cLin)),
                                 compTy),
                        sumPtr);
                    emitCountedLoop(K, [&](llvm::Value* k) {
                        llvm::Value* aLin = builder.CreateAdd(
                            builder.CreateMul(m, llvm::ConstantInt::get(i32, K)),
                            k);
                        llvm::Value* bLin = builder.CreateAdd(
                            builder.CreateMul(k, llvm::ConstantInt::get(i32, N)),
                            n);
                        llvm::Value* av = coerceTo(
                            builder.CreateLoad(a.elemType, coopElemPtr(a, aLin)),
                            compTy);
                        llvm::Value* bv = coerceTo(
                            builder.CreateLoad(b.elemType, coopElemPtr(b, bLin)),
                            compTy);
                        llvm::Value* prod = fp ? builder.CreateFMul(av, bv)
                                               : builder.CreateMul(av, bv);
                        llvm::Value* cur = builder.CreateLoad(compTy, sumPtr);
                        llvm::Value* nsum = fp ? builder.CreateFAdd(cur, prod)
                                               : builder.CreateAdd(cur, prod);
                        builder.CreateStore(nsum, sumPtr);
                    });
                    builder.CreateStore(
                        coerceTo(builder.CreateLoad(compTy, sumPtr), acc),
                        coopElemPtr(slot, cLin));
                });
            });
            return llvm::ConstantInt::get(i32, 0);
        }
        unsupported("CooperativeMatrix." + name + "() [software]");
    }

    // Assemble a <3 x float> from three scalar expressions (ray origin /
    // direction components), each coerced to f32.
    llvm::Value* makeVec3(const ExpressionPtr& x, const ExpressionPtr& y,
                          const ExpressionPtr& z) {
        auto* v3f = llvm::FixedVectorType::get(llvm::Type::getFloatTy(ctx), 3);
        llvm::Value* v = llvm::PoisonValue::get(v3f);
        v = builder.CreateInsertElement(v, toFloat(lowerExpr(x)), uint64_t(0));
        v = builder.CreateInsertElement(v, toFloat(lowerExpr(y)), uint64_t(1));
        v = builder.CreateInsertElement(v, toFloat(lowerExpr(z)), uint64_t(2),
                                        "ray.vec3");
        return v;
    }

    // Coerce a value to f32 — texture coords are floats, but an int expression
    // (e.g. a lane index used as a coordinate) is widened via signed conversion.
    llvm::Value* toFloat(llvm::Value* v) {
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        if (v->getType() == f32) return v;
        if (v->getType()->isFloatingPointTy())
            return builder.CreateFPCast(v, f32);
        return builder.CreateSIToFP(v, f32);
    }

    // Math.<fn>(...) inside a kernel — `cajeta-gpu` Stage B2, increment 1.
    // Only the subset that lowers *natively* on every backend
    // (NVPTX/AMDGPU/SPIR-V/CPU) with no device math-library link is admitted
    // here: sqrt/floor/ceil/trunc/round, abs, min/max, fma. The transcendentals
    // (sin/cos/tan/exp/log/pow) need per-backend device-lib linking (ocml on
    // AMD, libdevice on NVPTX — the same shape as Item 8's ockl.bc) and get a
    // clean diagnostic until that increment lands. Unlike the host path (which
    // forces f64 for Java-Math parity), this operates in the argument's FP type
    // — f32 is GPU-native and f64 would need the Vulkan Float64 capability.
    llvm::Value* lowerMathCall(const std::string& name,
                               const std::shared_ptr<MethodCallExpression>& mc) {
        const auto& args = mc->getParameters();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        // Vectorized math: a `Vector<T,N>` argument flows as `<N x T>`, so the
        // intrinsics/seam operate elementwise. asFp passes an FP scalar/vector
        // through and widens an int scalar/vector to f32 (matching lane count).
        auto asFp = [&](llvm::Value* v) -> llvm::Value* {
            llvm::Type* ty = v->getType();
            if (ty->isFPOrFPVectorTy()) return v;
            llvm::Type* target = f32;
            if (auto* vt = llvm::dyn_cast<llvm::FixedVectorType>(ty))
                target = llvm::FixedVectorType::get(f32, vt->getNumElements());
            return builder.CreateSIToFP(v, target);       // int -> f32
        };
        // Unary float intrinsics, native on all four backends.
        static const struct { const char* n; llvm::Intrinsic::ID id; } unary[] = {
            {"sqrt",  llvm::Intrinsic::sqrt},
            {"floor", llvm::Intrinsic::floor},
            {"ceil",  llvm::Intrinsic::ceil},
            {"trunc", llvm::Intrinsic::trunc},
            {"round", llvm::Intrinsic::round},
        };
        for (const auto& u : unary) {
            if (name == u.n) {
                if (args.size() != 1)
                    unsupported("Math." + name + " expects 1 argument");
                llvm::Value* x = asFp(lowerExpr(args[0].expression));
                llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
                    &mod, u.id, {x->getType()});
                return builder.CreateCall(fn, {x});
            }
        }
        if (name == "abs") {
            if (args.size() != 1) unsupported("Math.abs expects 1 argument");
            llvm::Value* x = lowerExpr(args[0].expression);
            if (x->getType()->isFloatingPointTy()) {
                llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
                    &mod, llvm::Intrinsic::fabs, {x->getType()});
                return builder.CreateCall(fn, {x});
            }
            llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
                &mod, llvm::Intrinsic::abs, {x->getType()});
            return builder.CreateCall(fn, {x, llvm::ConstantInt::getFalse(ctx)});
        }
        if (name == "min" || name == "max") {
            if (args.size() != 2)
                unsupported("Math." + name + " expects 2 arguments");
            llvm::Value* a = lowerExpr(args[0].expression);
            llvm::Value* b = lowerExpr(args[1].expression);
            bool fp = a->getType()->isFloatingPointTy()
                   || b->getType()->isFloatingPointTy();
            if (fp) {
                // Common FP type: f64 only if a double was used explicitly.
                llvm::Type* ft = (a->getType()->isDoubleTy()
                               || b->getType()->isDoubleTy())
                                   ? llvm::Type::getDoubleTy(ctx) : f32;
                auto toFt = [&](llvm::Value* v) -> llvm::Value* {
                    if (v->getType() == ft) return v;
                    if (v->getType()->isFloatingPointTy())
                        return builder.CreateFPCast(v, ft);
                    return builder.CreateSIToFP(v, ft);
                };
                a = toFt(a); b = toFt(b);
                llvm::Intrinsic::ID id = name == "max"
                    ? llvm::Intrinsic::maxnum : llvm::Intrinsic::minnum;
                llvm::Function* fn =
                    llvm::Intrinsic::getOrInsertDeclaration(&mod, id, {ft});
                return builder.CreateCall(fn, {a, b});
            }
            // Integer min/max: unify to the wider operand width, then choose
            // signed vs unsigned min/max + the matching extension by the operands'
            // signedness — smin/smax on unsigned values is wrong, e.g.
            // umin(0xFFFFFFFF, 1) must be 1, not 0xFFFFFFFF (L2).
            llvm::Type* it =
                a->getType()->getIntegerBitWidth()
                    >= b->getType()->getIntegerBitWidth()
                        ? a->getType() : b->getType();
            bool signedOp = exprSigned(args[0].expression) ||
                            exprSigned(args[1].expression);
            if (a->getType() != it)
                a = signedOp ? builder.CreateSExt(a, it) : builder.CreateZExt(a, it);
            if (b->getType() != it)
                b = signedOp ? builder.CreateSExt(b, it) : builder.CreateZExt(b, it);
            llvm::Intrinsic::ID id = name == "max"
                ? (signedOp ? llvm::Intrinsic::smax : llvm::Intrinsic::umax)
                : (signedOp ? llvm::Intrinsic::smin : llvm::Intrinsic::umin);
            llvm::Function* fn =
                llvm::Intrinsic::getOrInsertDeclaration(&mod, id, {it});
            return builder.CreateCall(fn, {a, b});
        }
        if (name == "fma") {
            if (args.size() != 3) unsupported("Math.fma expects 3 arguments");
            llvm::Value* a = asFp(lowerExpr(args[0].expression));
            llvm::Value* b = asFp(lowerExpr(args[1].expression));
            llvm::Value* c = asFp(lowerExpr(args[2].expression));
            llvm::Type* ft = a->getType();
            auto toA = [&](llvm::Value* v) -> llvm::Value* {
                return v->getType() == ft ? v : builder.CreateFPCast(v, ft);
            };
            b = toA(b); c = toA(c);
            llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
                &mod, llvm::Intrinsic::fma, {ft});
            return builder.CreateCall(fn, {a, b, c});
        }
        // B2 increment 2 — transcendentals. Operate in the argument's FP type
        // (f32-native, not the host's forced f64). These lower per backend: CPU
        // -> libm (sinf/…); Vulkan -> the SPIR-V backend maps the llvm.* trig/
        // exp/log intrinsics to OpExtInst GLSL.std.450 (Sin/Cos/Exp/Log/…); AMD/
        // NV realize through the device math library when present.
        static const std::set<std::string> unaryTransc = {
            "sin", "cos", "tan", "asin", "acos", "atan",
            "exp", "exp2", "log", "log2", "log10", "rsqrt"};
        if (unaryTransc.count(name)) {
            if (args.size() != 1)
                unsupported("Math." + name + " expects 1 argument");
            llvm::Value* x = asFp(lowerExpr(args[0].expression));
            return target.transcendental(builder, mod, name, {x});
        }
        if (name == "pow" || name == "atan2") {
            if (args.size() != 2)
                unsupported("Math." + name + " expects 2 arguments");
            llvm::Value* a = asFp(lowerExpr(args[0].expression));
            llvm::Value* b = asFp(lowerExpr(args[1].expression));
            // Broadcast a scalar exponent/operand to a vectorized base.
            if (a->getType()->isVectorTy() && !b->getType()->isVectorTy())
                b = vecops::splat(builder, b,
                    llvm::cast<llvm::FixedVectorType>(a->getType())->getNumElements());
            else if (b->getType()->isVectorTy() && !a->getType()->isVectorTy())
                a = vecops::splat(builder, a,
                    llvm::cast<llvm::FixedVectorType>(b->getType())->getNumElements());
            else if (b->getType() != a->getType())
                b = builder.CreateFPCast(b, a->getType());
            return target.transcendental(builder, mod, name, {a, b});
        }
        unsupported("Math." + name + " is not available in a kernel on device");
    }

    // Resolve a call `name(args)` / `Cls.name(args)` to a sibling @Device method
    // of the kernel's class (first cut: same class only), matched by arity.
    MethodPtr resolveDeviceMethod(
            const std::string& recv, const std::string& name,
            const std::shared_ptr<MethodCallExpression>& mc) {
        if (!cls || !deviceFns) return nullptr;
        if (!recv.empty()) {
            // `Cls.helper(...)` — accept only the kernel's own (simple) class
            // name (the last segment of the qualified name).
            std::string q = cls->toCanonical();
            std::string simple = q.substr(q.find_last_of('.') + 1);
            if (recv != simple) return nullptr;
        }
        unsigned argc = (unsigned) mc->getParameters().size();
        for (auto& kv : cls->getMethods()) {
            const MethodPtr& m = kv.second;
            if (!m || m->getName() != name || !isDevice(*m)) continue;
            if (m->getParameters().size() != argc) continue;
            return m;
        }
        return nullptr;
    }

    // Lower a @Device method to a device function (scalar params + scalar/void
    // return), cached. alwaysinline so every backend folds the call away. A
    // nullptr cache entry means it's mid-lowering → a recursive call (rejected).
    llvm::Function* lowerDeviceFn(const MethodPtr& m) {
        auto it = deviceFns->find(m.get());
        if (it != deviceFns->end()) {
            if (!it->second) unsupported("recursive @Device call");
            return it->second;
        }
        (*deviceFns)[m.get()] = nullptr;            // mark in-progress

        std::vector<LoweringTarget::KernelParam> params = collectParams(m, ctx);
        std::vector<llvm::Type*> tys;
        tys.reserve(params.size());
        for (auto& p : params) {
            // A Buffer<T> param takes the buffer base BY VALUE (the backend's
            // pointer/handle), matching the caller's bufferBases entry; scalars
            // by value. The helper is alwaysinline, so the base flows straight
            // through inlining to the kernel's real buffer access (Item 2).
            tys.push_back(p.isBuffer ? target.bufferParamType(mod, p.type)
                                     : p.type);
        }
        llvm::Type* retTy = nullptr;
        if (auto rt = m->getReturnType()) {
            retTy = deviceScalarType(rt, ctx);
            if (!retTy) retTy = deviceVectorType(rt, ctx);     // Vector<T,N>-returning
            // S8: a value-type-returning @Device operator returns its flat device
            // struct by value (built by lowerNewValueType, returned as an SSA
            // aggregate — no pointer, SPIR-V-logical-safe).
            if (!retTy && rt->isValueType())
                retTy = deviceStructInfo(rt, ctx).type;
        }
        if (!retTy) retTy = llvm::Type::getVoidTy(ctx);
        // Lower the body in the context of the method's OWN class, not the
        // kernel's: a @Device helper may live in a different class (e.g. the core
        // SoftwareRayQuery the ray-query call site dispatches to). For an
        // ordinary sibling helper this is the kernel class, unchanged.
        auto owner = m->getParent() ? m->getParent() : cls;
        auto* fnTy = llvm::FunctionType::get(retTy, tys, /*vararg=*/false);
        std::string fname = "__cajeta_xpu_dev." +
            (owner ? owner->toCanonical() + "." : std::string()) + m->getName();
        auto* hfn = llvm::Function::Create(
            fnTy, llvm::GlobalValue::InternalLinkage, fname, &mod);
        hfn->addFnAttr(llvm::Attribute::AlwaysInline);
        unsigned i = 0;
        for (auto& p : params) hfn->getArg(i++)->setName(p.name);

        DeviceLowerer sub(mod, hfn, target);
        sub.setParams(params);
        sub.setParamsAsArgs(true);   // helper params are plain fn args, not
                                     // kernel descriptors/SSBOs (Vulkan)
        sub.setDeviceContext(owner, deviceFns);
        sub.lowerBody(m);

        (*deviceFns)[m.get()] = hfn;
        return hfn;
    }

    // S8 device operator dispatch. When the LHS resolves to a @ValueType, route
    // `a OP b` to the class's static @Device operator (or a comparison derived
    // from it) instead of the native scalar/vector path. Reuses the SAME S6
    // dispatch/derivation policy as the host (opdispatch::dispatchBinaryOperator)
    // — only the resolve+invoke and negate callbacks are device-specific: resolve
    // the operator method, require @Device (pure), lower it via lowerDeviceFn
    // (alwaysinline aggregate-param/scalar-return helper), and emit the call.
    // Returns nullptr (fall through) when the LHS isn't a value type or no
    // matching @Device operator exists. v1 covers operators that RETURN a scalar
    // (e.g. ==, < and their derivations) — a value-type-returning device operator
    // (aggregate construction + return inside a kernel) is the next S8 increment.
    // The @ValueType of an operand expression in the device lowerer: a bare
    // value-type-typed name (param/local, tracked in valueTypeNames) or the AST
    // resolvedType when available. Returns null for non-value-type operands.
    CajetaTypePtr operandValueType(const ExpressionPtr& e) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(e)) {
            auto it = valueTypeNames.find(id->getTextValue());
            if (it != valueTypeNames.end()) return it->second;
        }
        return e ? e->getResolvedType() : nullptr;
    }

    llvm::Value* lowerValueTypeBinaryOp(
            const std::shared_ptr<BinaryOpExpression>& bin,
            const ExpressionPtr& le, const ExpressionPtr& re, BinaryOp op) {
        // Kernel bodies aren't host-type-resolved, so prefer the device
        // lowerer's own value-type-name map (params/locals), falling back to the
        // AST resolvedType when present. isValueType() resolves through
        // canonicalMap (born-correct archive makes the flag reliable).
        CajetaTypePtr lhsType = operandValueType(le);
        if (!lhsType || !lhsType->isValueType()) return nullptr;
        auto lhsClass = std::dynamic_pointer_cast<CajetaClass>(lhsType);
        if (!lhsClass) return nullptr;
        if (!opdispatch::binaryOpSymbol(op)) return nullptr;
        CajetaTypePtr rhsType = operandValueType(re);
        if (!rhsType) rhsType = lhsType;   // homogeneous op is the common case
        // Lower both operands once (whole aggregate values).
        llvm::Value* lv = lowerExpr(le);
        llvm::Value* rv = lowerExpr(re);

        auto tryInvoke = [&](std::string name, bool swap)
                -> std::pair<bool, llvm::Value*> {
            std::vector<cajeta::ParameterEntry> ents;
            if (swap) {
                ents.emplace_back(rhsType, "", nullptr);
                ents.emplace_back(lhsType, "", nullptr);
            } else {
                ents.emplace_back(lhsType, "", nullptr);
                ents.emplace_back(rhsType, "", nullptr);
            }
            MethodPtr m = lhsClass->resolveMethod(
                name, ents, /*isConstructor=*/false, /*floatingParams=*/false);
            if (!m || !isDevice(*m)) return {false, nullptr};
            llvm::Function* opFn = lowerDeviceFn(m);
            std::vector<llvm::Value*> args =
                swap ? std::vector<llvm::Value*>{rv, lv}
                     : std::vector<llvm::Value*>{lv, rv};
            return {true, builder.CreateCall(opFn, args)};
        };
        auto negate = [&](llvm::Value* v) -> llvm::Value* {
            return builder.CreateNot(toI1(v), "derived.not");
        };
        std::pair<bool, llvm::Value*> disp =
            opdispatch::dispatchBinaryOperator(op, tryInvoke, negate);
        if (disp.first) return disp.second;
        // The LHS IS a value type but no @Device operator (direct or derived)
        // applied — falling through to the scalar path would emit an ICmp on the
        // aggregate and crash. Surface a clear diagnostic instead.
        unsupported("no @Device 'operator" +
                    std::string(opdispatch::binaryOpSymbol(op)) +
                    "' for value type '" + lhsType->toCanonical() +
                    "' in kernel (declare it @Device, or the comparison it "
                    "derives from)");
    }

    // B1: `*` on a matrix local. `a * b` -> matmul (Matrix*Matrix, K checked),
    // matVec (Matrix*Vector), or scale (Matrix*scalar). Returns nullptr when the
    // op isn't `*` or the LHS isn't a matrix local (caller falls through). Shapes
    // come from matrixShapes (matmul) / vectorSlotType (matVec).
    llvm::Value* lowerMatrixMul(const ExpressionPtr& le, const ExpressionPtr& re,
                                BinaryOp op) {
        if (op != BINARY_OP_MUL) return nullptr;
        auto lid = std::dynamic_pointer_cast<IdentifierExpression>(le);
        if (!lid) return nullptr;
        auto lsh = matrixShapes.find(lid->getTextValue());
        if (lsh == matrixShapes.end()) return nullptr;
        unsigned R = lsh->second.first, K = lsh->second.second;
        llvm::Value* l = lowerExpr(le);
        bool isFloat = llvm::cast<llvm::FixedVectorType>(l->getType())
                           ->getElementType()->isFloatingPointTy();
        if (auto rid = std::dynamic_pointer_cast<IdentifierExpression>(re)) {
            auto rsh = matrixShapes.find(rid->getTextValue());
            if (rsh != matrixShapes.end()) {
                if (K != rsh->second.first)
                    unsupported("matrix multiply shape mismatch in kernel");
                llvm::Value* r = lowerExpr(re);
                return matops::matmul(builder, l, R, K, r, rsh->second.second,
                                      isFloat);
            }
            if (llvm::FixedVectorType* vt = vectorSlotType(rid->getTextValue())) {
                if (vt->getNumElements() != K)
                    unsupported("matrix-vector shape mismatch in kernel");
                llvm::Value* r = lowerExpr(re);
                return matops::matVec(builder, l, R, K, r, isFloat);
            }
        }
        // RHS scalar -> scale (a vector RHS that isn't a known matrix/vector
        // local would be ambiguous; require a scalar).
        llvm::Value* r = lowerExpr(re);
        if (r->getType()->isVectorTy())
            unsupported("matrix `*` RHS must be a matrix, vector, or scalar");
        return matops::scale(builder, l, r, isFloat);
    }

    // `*` on a quaternion local: Quaternion*Quaternion -> Hamilton product;
    // Quaternion*Vector<T,3> -> the rotated vector. Returns nullptr when the LHS
    // isn't a quaternion local (caller falls through).
    llvm::Value* lowerQuaternionMul(const ExpressionPtr& le,
                                    const ExpressionPtr& re, BinaryOp op) {
        if (op != BINARY_OP_MUL) return nullptr;
        auto lid = std::dynamic_pointer_cast<IdentifierExpression>(le);
        if (!lid || !quaternionLocals.count(lid->getTextValue())) return nullptr;
        llvm::Value* l = lowerExpr(le);
        if (auto rid = std::dynamic_pointer_cast<IdentifierExpression>(re)) {
            if (quaternionLocals.count(rid->getTextValue()))
                return quatops::multiply(builder, l, lowerExpr(re));
        }
        llvm::Value* r = lowerExpr(re);
        if (!r->getType()->isVectorTy()
                || llvm::cast<llvm::FixedVectorType>(r->getType())
                       ->getNumElements() != 3)
            unsupported("Quaternion `*` requires a Quaternion or a Vector<T,3>");
        return quatops::rotate(builder, l, r);
    }

    llvm::Value* lowerBinaryOp(const std::shared_ptr<BinaryOpExpression>& bin) {
        BinaryOp op = bin->getBinaryOp();
        // && / || evaluate lazily — control flow, not eager operands.
        if (op == BINARY_OP_LOGAND || op == BINARY_OP_LOGOR)
            return lowerLogical(bin);
        ExpressionPtr le = exprChild(bin, 0), re = exprChild(bin, 1);
        // B1: `*` on a matrix local is matrix multiply / matrix-vector / scalar
        // scale (NOT element-wise). + - / are element-wise and lower correctly
        // through the flat-vector path below (same `<R*C x T>` op), so only `*`
        // needs interception.
        if (llvm::Value* mm = lowerMatrixMul(le, re, op))
            return mm;
        // `*` on a quaternion local is the Hamilton product / vector rotation
        // (NOT element-wise fmul); `+ -` element-wise lower through the flat path.
        if (llvm::Value* qm = lowerQuaternionMul(le, re, op))
            return qm;
        // S8: a @ValueType operand dispatches to the (pure) @Device operator on
        // its class — emitted as an alwaysinline call the backend inliner folds
        // to flat SSA. Falls through to the native scalar/vector path otherwise.
        if (llvm::Value* vt = lowerValueTypeBinaryOp(bin, le, re, op))
            return vt;
        llvm::Value* l = lowerExpr(le);
        llvm::Value* r = lowerExpr(re);
        // The LHS drives the operation's signedness — matches the language's
        // "lhs type is the result type" rule (BinaryOpExpression::resolveTypes)
        // and keeps an unsigned `i >> 2` a logical shift despite the literal.
        bool sign = exprSigned(le);
        return applyBinOp(op, l, r, sign,
                          l->getType()->isFloatingPointTy() ||
                          r->getType()->isFloatingPointTy());
    }

    // Short-circuit lhs && rhs / lhs || rhs via branch + phi.
    llvm::Value* lowerLogical(const std::shared_ptr<BinaryOpExpression>& bin) {
        bool isAnd = bin->getBinaryOp() == BINARY_OP_LOGAND;
        llvm::Value* l = toI1(lowerExpr(exprChild(bin, 0)));
        llvm::BasicBlock* lhsEnd = builder.GetInsertBlock();  // snapshot
        auto* rhsBB  = llvm::BasicBlock::Create(ctx, "logic.rhs", fn);
        auto* doneBB = llvm::BasicBlock::Create(ctx, "logic.done", fn);
        if (isAnd) builder.CreateCondBr(l, rhsBB, doneBB);   // AND: false short-circuits
        else       builder.CreateCondBr(l, doneBB, rhsBB);   // OR:  true short-circuits
        builder.SetInsertPoint(rhsBB);
        llvm::Value* r = toI1(lowerExpr(exprChild(bin, 1)));
        llvm::BasicBlock* rhsEnd = builder.GetInsertBlock();
        builder.CreateBr(doneBB);
        builder.SetInsertPoint(doneBB);
        llvm::PHINode* phi = builder.CreatePHI(llvm::Type::getInt1Ty(ctx), 2);
        phi->addIncoming(llvm::ConstantInt::getBool(ctx, !isAnd), lhsEnd);
        phi->addIncoming(r, rhsEnd);
        return phi;
    }

    // Core binary op on two lowered values, after width/fp unification.
    // bfloat16 is a STORAGE format — native bfloat arithmetic is a vendor
    // extension (SPV_INTEL_bfloat16_arithmetic), absent on most GPUs. So compute
    // bfloat in f32 (widen / op / narrow), the standard "bf16 storage, f32
    // compute" model — portable across CPU/Vulkan/AMD. float16 needs no such
    // treatment (native f16 arithmetic is portable). Comparisons (i1 result) are
    // not narrowed.
    llvm::Value* applyBinOp(BinaryOp op, llvm::Value* l, llvm::Value* r,
                            bool sign, bool fpHint) {
        unifyOperands(l, r, sign);
        if (l->getType()->getScalarType()->isBFloatTy()) {
            llvm::Type* bfTy = l->getType();
            llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
            llvm::Type* compTy = bfTy->isVectorTy()
                ? (llvm::Type*) llvm::FixedVectorType::get(
                      f32, llvm::cast<llvm::FixedVectorType>(bfTy)->getNumElements())
                : f32;
            llvm::Value* res = applyBinOpInner(
                op, builder.CreateFPExt(l, compTy),
                builder.CreateFPExt(r, compTy), sign, fpHint);
            if (res->getType()->getScalarType()->isFloatTy())   // FP arith result
                return builder.CreateFPTrunc(res, bfTy);        // narrow back to bfloat
            return res;                                         // comparison mask
        }
        return applyBinOpInner(op, l, r, sign, fpHint);
    }

    llvm::Value* applyBinOpInner(BinaryOp op, llvm::Value* l, llvm::Value* r,
                                 bool sign, bool /*fpHint*/) {
        // isFPOrFPVectorTy so element-wise `<N x float>` arithmetic routes to
        // CreateFAdd/… (a bare isFloatingPointTy is false for a vector type).
        bool fp = l->getType()->isFPOrFPVectorTy();
        switch (op) {
            case BINARY_OP_ADD: return fp ? builder.CreateFAdd(l, r) : builder.CreateAdd(l, r);
            case BINARY_OP_SUB: return fp ? builder.CreateFSub(l, r) : builder.CreateSub(l, r);
            case BINARY_OP_MUL: return fp ? builder.CreateFMul(l, r) : builder.CreateMul(l, r);
            case BINARY_OP_DIV: return fp ? builder.CreateFDiv(l, r)
                                          : (sign ? builder.CreateSDiv(l, r) : builder.CreateUDiv(l, r));
            case BINARY_OP_MOD: return fp ? builder.CreateFRem(l, r)
                                          : (sign ? builder.CreateSRem(l, r) : builder.CreateURem(l, r));
            case BINARY_OP_BITAND: return builder.CreateAnd(l, r);
            case BINARY_OP_BITOR:  return builder.CreateOr(l, r);
            case BINARY_OP_BITXOR: return builder.CreateXor(l, r);
            case BINARY_OP_SHIFTLEFT:   return builder.CreateShl(l, r);
            case BINARY_OP_SHIFTRIGHT:  return sign ? builder.CreateAShr(l, r) : builder.CreateLShr(l, r);
            case BINARY_OP_USHIFTRIGHT: return builder.CreateLShr(l, r);
            case BINARY_OP_LT: return fp ? builder.CreateFCmpOLT(l, r) : (sign ? builder.CreateICmpSLT(l, r) : builder.CreateICmpULT(l, r));
            case BINARY_OP_LE: return fp ? builder.CreateFCmpOLE(l, r) : (sign ? builder.CreateICmpSLE(l, r) : builder.CreateICmpULE(l, r));
            case BINARY_OP_GT: return fp ? builder.CreateFCmpOGT(l, r) : (sign ? builder.CreateICmpSGT(l, r) : builder.CreateICmpUGT(l, r));
            case BINARY_OP_GE: return fp ? builder.CreateFCmpOGE(l, r) : (sign ? builder.CreateICmpSGE(l, r) : builder.CreateICmpUGE(l, r));
            case BINARY_OP_EQ: return fp ? builder.CreateFCmpOEQ(l, r) : builder.CreateICmpEQ(l, r);
            case BINARY_OP_NE: return fp ? builder.CreateFCmpONE(l, r) : builder.CreateICmpNE(l, r);
            default: unsupported("binary operator in kernel body");
        }
    }

    // Bring two scalars to a common type: fp dominates int (int→fp); among
    // ints, extend the narrower to the wider (sext if signed, else zext).
    void unifyOperands(llvm::Value*& l, llvm::Value*& r, bool sign) {
        llvm::Type* lt = l->getType();
        llvm::Type* rt = r->getType();
        if (lt == rt) return;
        // Vector op scalar: broadcast the scalar to the vector's shape (coercing
        // its element type first). Two different-shape vectors are a type error
        // the verifier catches; same-shape vectors took the `lt == rt` fast path.
        bool lVec = lt->isVectorTy(), rVec = rt->isVectorTy();
        if (lVec != rVec) {
            auto* vt = llvm::cast<llvm::FixedVectorType>(lVec ? lt : rt);
            llvm::Type* elemTy = vt->getElementType();
            unsigned n = vt->getNumElements();
            if (lVec) {
                r = vecops::splat(builder,
                                  vecops::coerceScalar(builder, r, elemTy), n);
            } else {
                l = vecops::splat(builder,
                                  vecops::coerceScalar(builder, l, elemTy), n);
            }
            return;
        }
        bool lFp = lt->isFloatingPointTy(), rFp = rt->isFloatingPointTy();
        if (lFp || rFp) {
            llvm::Type* fT = lFp ? lt : rt;
            if (!lFp)        l = sign ? builder.CreateSIToFP(l, fT) : builder.CreateUIToFP(l, fT);
            else if (lt != fT) l = builder.CreateFPCast(l, fT);
            if (!rFp)        r = sign ? builder.CreateSIToFP(r, fT) : builder.CreateUIToFP(r, fT);
            else if (rt != fT) r = builder.CreateFPCast(r, fT);
            return;
        }
        llvm::Type* wide = lt->getIntegerBitWidth() >= rt->getIntegerBitWidth() ? lt : rt;
        l = builder.CreateIntCast(l, wide, sign);
        r = builder.CreateIntCast(r, wide, sign);
    }

    // ---- helpers --------------------------------------------------------

    ExpressionPtr exprChild(const std::shared_ptr<Expression>& e, size_t i) {
        if (e->getChildren().size() <= i) unsupported("missing operand");
        return std::dynamic_pointer_cast<Expression>(e->getChildren()[i]);
    }

    bool exprSigned(const ExpressionPtr& e) {
        if (auto* f = structFieldOf(e)) return f->isSigned;
        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(e)) {
            auto it = signedness.find(id->getTextValue());
            return it != signedness.end() ? it->second : true;
        }
        if (auto ai = std::dynamic_pointer_cast<ArrayIndexExpression>(e)) {
            if (auto b = std::dynamic_pointer_cast<IdentifierExpression>(
                    exprChild(ai, 0))) {
                auto it = bufferElemSigned.find(b->getTextValue());
                if (it != bufferElemSigned.end()) return it->second;
            }
            return true;
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

// ---- LoweringTarget default hooks (NVPTX/AMDGPU pointer-arg model) --------
//
// These reproduce the pre-Vulkan signature/parameter behavior, so NVPTX and
// AMDGPU inherit them unchanged. Vulkan overrides all three (SpirvTarget).

llvm::Function* LoweringTarget::createKernel(
    llvm::Module& m, const std::string& name,
    const std::vector<KernelParam>& params) {
    llvm::LLVMContext& ctx = m.getContext();
    // Buffer<T> / arrays -> addrspace(1) pointers; primitives by value.
    std::vector<llvm::Type*> tys;
    tys.reserve(params.size());
    for (auto& p : params) {
        if (p.isBuffer)
            tys.push_back(bufferParamType(m, p.type));
        else if (p.isTexture || p.isImage)
            tys.push_back(textureParamType(m));   // texture/image handle (ptr/i64)
        else
            tys.push_back(p.type);                 // scalar / sampler {i32,i32}
    }
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), tys,
                                         /*vararg=*/false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage,
                                      name, &m);
    unsigned i = 0;
    for (auto& p : params) fn->getArg(i++)->setName(p.name);
    decorateKernel(fn, m);  // calling convention + any kernel-marker metadata
    return fn;
}

llvm::Value* LoweringTarget::materializeParam(llvm::IRBuilderBase& /*b*/,
                                              llvm::Module& /*m*/,
                                              llvm::Function* fn, unsigned idx,
                                              const KernelParam& /*p*/) {
    return fn->getArg(idx);  // the value IS the argument
}

llvm::Value* LoweringTarget::bufferElementPtr(llvm::IRBuilderBase& b,
                                              llvm::Module& /*m*/,
                                              llvm::Value* base,
                                              llvm::Type* elemTy,
                                              llvm::Value* index) {
    // addrspace-preserving GEP — the base pointer carries its address space
    // (1 for global buffers, 3 for shared globals); correct on NVPTX/AMDGPU.
    return b.CreateGEP(elemTy, base, {index}, "idx");
}

llvm::Value* LoweringTarget::bufferArrayElement(llvm::IRBuilderBase& b,
                                                llvm::Module& /*m*/,
                                                llvm::Function* /*fn*/,
                                                unsigned /*binding*/,
                                                llvm::Value* arrayBase,
                                                llvm::Type* /*elemTy*/,
                                                llvm::Value* descIndex) {
    // Pointer backends (CPU / NVPTX / AMDGPU): `arrayBase` points to the
    // [i64 count, i64 h0 …] handle array the launch marshalled. bufs[idx] is the
    // (1 + idx)-th handle reinterpreted as a device pointer; the inner [i] then
    // GEPs it via the default bufferElementPtr. (Vulkan overrides to bind a
    // descriptor-array element instead.)
    if (!arrayBase) return nullptr;
    llvm::LLVMContext& ctx = b.getContext();
    llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
    llvm::Value* idx64 = b.CreateIntCast(descIndex, i64, /*isSigned=*/false);
    llvm::Value* slotIdx =
        b.CreateAdd(idx64, llvm::ConstantInt::get(i64, 1), "bufarr.slotidx");
    llvm::Value* hPtr = b.CreateGEP(i64, arrayBase, {slotIdx}, "bufarr.h.ptr");
    llvm::Value* handle = b.CreateLoad(i64, hPtr, "bufarr.h");
    return b.CreateIntToPtr(handle, llvm::PointerType::get(ctx, 0),
                            "bufarr.base");
}

llvm::Value* LoweringTarget::sampleTexture(llvm::IRBuilderBase& /*b*/,
                                           llvm::Module& /*m*/,
                                           llvm::Value* /*texHandle*/,
                                           llvm::Value* /*samplerHandle*/,
                                           llvm::Value* /*u*/,
                                           llvm::Value* /*v*/,
                                           llvm::Value* /*lod*/) {
    // Only backends with hardware image sampling override this (CPU emulation,
    // Vulkan OpImageSampleExplicitLod, AMD image_sample). The default rejects a
    // tex.sample() in a kernel lowered for a backend that has not implemented it.
    throw cajeta::Exception(
        "XPU kernel lowering: texture sampling not supported on backend '" +
        std::string(name()) + "'", "XPU-N01");
}

llvm::Value* LoweringTarget::fetchTexture(llvm::IRBuilderBase& /*b*/,
                                          llvm::Module& /*m*/,
                                          llvm::Value* /*texHandle*/,
                                          llvm::Value* /*x*/,
                                          llvm::Value* /*y*/,
                                          llvm::Type* /*texelTy*/,
                                          llvm::Value* /*lod*/) {
    // Only backends with an unfiltered image read override this (CPU exact
    // texel read, Vulkan OpImageFetch, AMD __ockl_image_load_2D). The default
    // rejects a tex.fetch() in a kernel lowered for a backend (e.g. NVPTX in v1)
    // that has not implemented it.
    throw cajeta::Exception(
        "XPU kernel lowering: texture fetch (texelFetch) not supported on "
        "backend '" + std::string(name()) + "'", "XPU-N01");
}

llvm::Value* LoweringTarget::sampleTexture3D(llvm::IRBuilderBase& /*b*/,
                                             llvm::Module& /*m*/,
                                             llvm::Value* /*texHandle*/,
                                             llvm::Value* /*samplerHandle*/,
                                             llvm::Value* /*u*/,
                                             llvm::Value* /*v*/,
                                             llvm::Value* /*w*/) {
    // Only backends with 3-D hardware image sampling override this.
    throw cajeta::Exception(
        "XPU kernel lowering: 3-D texture sampling not supported on backend '" +
        std::string(name()) + "'", "XPU-N01");
}

llvm::Value* LoweringTarget::fetchTexture3D(llvm::IRBuilderBase& /*b*/,
                                            llvm::Module& /*m*/,
                                            llvm::Value* /*texHandle*/,
                                            llvm::Value* /*x*/,
                                            llvm::Value* /*y*/,
                                            llvm::Value* /*z*/,
                                            llvm::Type* /*texelTy*/) {
    // Only backends with a 3-D unfiltered image read override this.
    throw cajeta::Exception(
        "XPU kernel lowering: 3-D texture fetch not supported on backend '" +
        std::string(name()) + "'", "XPU-N01");
}

llvm::Value* LoweringTarget::sampleTexture1D(llvm::IRBuilderBase& /*b*/,
                                             llvm::Module& /*m*/,
                                             llvm::Value* /*texHandle*/,
                                             llvm::Value* /*samplerHandle*/,
                                             llvm::Value* /*u*/) {
    // Only backends with 1-D hardware image sampling override this.
    throw cajeta::Exception(
        "XPU kernel lowering: 1-D texture sampling not supported on backend '" +
        std::string(name()) + "'", "XPU-N01");
}

llvm::Value* LoweringTarget::fetchTexture1D(llvm::IRBuilderBase& /*b*/,
                                            llvm::Module& /*m*/,
                                            llvm::Value* /*texHandle*/,
                                            llvm::Value* /*x*/,
                                            llvm::Type* /*texelTy*/) {
    // Only backends with a 1-D unfiltered image read override this.
    throw cajeta::Exception(
        "XPU kernel lowering: 1-D texture fetch not supported on backend '" +
        std::string(name()) + "'", "XPU-N01");
}

llvm::Value* LoweringTarget::sampleTexture2DArray(llvm::IRBuilderBase& /*b*/,
                                                  llvm::Module& /*m*/,
                                                  llvm::Value* /*texHandle*/,
                                                  llvm::Value* /*samplerHandle*/,
                                                  llvm::Value* /*u*/,
                                                  llvm::Value* /*v*/,
                                                  llvm::Value* /*layer*/) {
    // Only backends with layered (2-D array) image sampling override this.
    throw cajeta::Exception(
        "XPU kernel lowering: 2-D array texture sampling not supported on "
        "backend '" + std::string(name()) + "'", "XPU-N01");
}

llvm::Value* LoweringTarget::fetchTexture2DArray(llvm::IRBuilderBase& /*b*/,
                                                 llvm::Module& /*m*/,
                                                 llvm::Value* /*texHandle*/,
                                                 llvm::Value* /*x*/,
                                                 llvm::Value* /*y*/,
                                                 llvm::Value* /*layer*/,
                                                 llvm::Type* /*texelTy*/) {
    // Only backends with a layered unfiltered image read override this.
    throw cajeta::Exception(
        "XPU kernel lowering: 2-D array texture fetch not supported on "
        "backend '" + std::string(name()) + "'", "XPU-N01");
}

llvm::Value* LoweringTarget::sampleTextureCube(llvm::IRBuilderBase& /*b*/,
                                               llvm::Module& /*m*/,
                                               llvm::Value* /*texHandle*/,
                                               llvm::Value* /*samplerHandle*/,
                                               llvm::Value* /*x*/,
                                               llvm::Value* /*y*/,
                                               llvm::Value* /*z*/) {
    // Only backends with cube-map image sampling override this.
    throw cajeta::Exception(
        "XPU kernel lowering: cube texture sampling not supported on "
        "backend '" + std::string(name()) + "'", "XPU-N01");
}

void LoweringTarget::storeImage(llvm::IRBuilderBase& /*b*/,
                                llvm::Module& /*m*/,
                                llvm::Value* /*imgHandle*/,
                                llvm::Value* /*x*/, llvm::Value* /*y*/,
                                llvm::Value* /*value*/) {
    // Only backends with hardware storage-image write override this (Vulkan
    // OpImageWrite). The default rejects an img.store() in a kernel lowered for
    // a backend that has not implemented writable images.
    throw cajeta::Exception(
        "XPU kernel lowering: storage-image write not supported on backend '" +
        std::string(name()) + "'", "XPU-N01");
}

llvm::Value* LoweringTarget::loadImage(llvm::IRBuilderBase& /*b*/,
                                       llvm::Module& /*m*/,
                                       llvm::Value* /*imgHandle*/,
                                       llvm::Value* /*x*/, llvm::Value* /*y*/) {
    // Only backends with hardware storage-image read override this (Vulkan
    // OpImageRead). The default rejects an img.load() in a kernel lowered for a
    // backend that has not implemented writable images.
    throw cajeta::Exception(
        "XPU kernel lowering: storage-image read not supported on backend '" +
        std::string(name()) + "'", "XPU-N01");
}

// Default transcendental: the matching `llvm.*` intrinsic. Correct on CPU (libm)
// and Vulkan (SPIR-V backend -> OpExtInst GLSL.std.450). AMD overrides this.
llvm::Value* LoweringTarget::transcendental(
    llvm::IRBuilderBase& b, llvm::Module& m, const std::string& name,
    llvm::ArrayRef<llvm::Value*> args) {
    llvm::Type* ft = args[0]->getType();
    if (name == "rsqrt") {
        llvm::Function* sq = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::sqrt, {ft});
        return b.CreateFDiv(llvm::ConstantFP::get(ft, 1.0),
                            b.CreateCall(sq, {args[0]}), "rsqrt");
    }
    static const std::map<std::string, llvm::Intrinsic::ID> ids = {
        {"sin", llvm::Intrinsic::sin}, {"cos", llvm::Intrinsic::cos},
        {"tan", llvm::Intrinsic::tan}, {"asin", llvm::Intrinsic::asin},
        {"acos", llvm::Intrinsic::acos}, {"atan", llvm::Intrinsic::atan},
        {"exp", llvm::Intrinsic::exp}, {"exp2", llvm::Intrinsic::exp2},
        {"log", llvm::Intrinsic::log}, {"log2", llvm::Intrinsic::log2},
        {"log10", llvm::Intrinsic::log10}, {"pow", llvm::Intrinsic::pow},
        {"atan2", llvm::Intrinsic::atan2},
    };
    auto it = ids.find(name);
    if (it == ids.end())
        throw cajeta::Exception("XPU: unknown transcendental '" + name + "'",
                                "XPU-N01");
    llvm::Function* fn =
        llvm::Intrinsic::getOrInsertDeclaration(&m, it->second, {ft});
    return b.CreateCall(fn, std::vector<llvm::Value*>(args.begin(), args.end()));
}

// Default integer dot: the portable widening reduce (correct on CPU/AMD/NVIDIA).
// Vulkan overrides this to emit the DP4a op (llvm.spv.dot4add.*).
llvm::Value* LoweringTarget::integerDot4x8(
    llvm::IRBuilderBase& b, llvm::Module& /*m*/, llvm::Value* a, llvm::Value* c,
    llvm::Value* acc, bool isSigned) {
    return vecops::idotWiden(b, a, c, acc, isSigned);
}

// Default float atomic: a relaxed, system-scope atomicrmw. Selects the native
// global FP atomic on AMDGPU/NVPTX and a lock/cmpxchg on CPU. Vulkan overrides
// (it needs Device scope + AcquireRelease to satisfy spirv-val).
llvm::Value* LoweringTarget::atomicFloatRMW(
    llvm::IRBuilderBase& b, llvm::Module& /*m*/, AtomicFloatOp op,
    llvm::Value* ptr, llvm::Value* value) {
    llvm::AtomicRMWInst::BinOp binop =
        op == AtomicFloatOp::Add ? llvm::AtomicRMWInst::FAdd
      : op == AtomicFloatOp::Min ? llvm::AtomicRMWInst::FMin
                                 : llvm::AtomicRMWInst::FMax;
    return b.CreateAtomicRMW(binop, ptr, value, llvm::MaybeAlign(),
                             llvm::AtomicOrdering::Monotonic);
}

// Map an AtomicIntOp (+ signedness for min/max) to the LLVM atomicrmw BinOp. The
// SPIR-V backend picks OpAtomicIAdd/ISub/And/Or/Xor/Exchange/SMin/UMin/SMax/UMax
// from this; AMDGPU/NVPTX select native global atomics; CPU lowers to locked ops.
static llvm::AtomicRMWInst::BinOp atomicIntBinOp(
        LoweringTarget::AtomicIntOp op, bool isSigned) {
    using A = llvm::AtomicRMWInst;
    using O = LoweringTarget::AtomicIntOp;
    switch (op) {
        case O::Add:      return A::Add;
        case O::Sub:      return A::Sub;
        case O::And:      return A::And;
        case O::Or:       return A::Or;
        case O::Xor:      return A::Xor;
        case O::Exchange: return A::Xchg;
        case O::Min:      return isSigned ? A::Min : A::UMin;
        case O::Max:      return isSigned ? A::Max : A::UMax;
    }
    return A::Add;  // unreachable
}

// Default integer atomic: a relaxed, system-scope atomicrmw (native on
// AMDGPU/NVPTX, locked on CPU). Vulkan overrides for Device scope + AcquireRelease.
llvm::Value* LoweringTarget::atomicIntRMW(
    llvm::IRBuilderBase& b, llvm::Module& /*m*/, AtomicIntOp op,
    llvm::Value* ptr, llvm::Value* value, bool isSigned) {
    return b.CreateAtomicRMW(atomicIntBinOp(op, isSigned), ptr, value,
                             llvm::MaybeAlign(), llvm::AtomicOrdering::Monotonic);
}

// Default compare-exchange: a relaxed cmpxchg; returns the OLD value (element 0
// of the {value, success} aggregate). Vulkan overrides the orderings/scope.
llvm::Value* LoweringTarget::atomicCompareExchange(
    llvm::IRBuilderBase& b, llvm::Module& /*m*/, llvm::Value* ptr,
    llvm::Value* expected, llvm::Value* desired) {
    llvm::Value* pair = b.CreateAtomicCmpXchg(
        ptr, expected, desired, llvm::MaybeAlign(),
        llvm::AtomicOrdering::Monotonic, llvm::AtomicOrdering::Monotonic);
    return b.CreateExtractValue(pair, 0, "atomic.cas.old");
}

// Default shader clock: llvm.readcyclecounter (CPU rdtsc). AMD/NVPTX/Vulkan
// override with their native device clock.
llvm::Value* LoweringTarget::readClock(llvm::IRBuilderBase& b,
                                       llvm::Module& m) {
    llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
        &m, llvm::Intrinsic::readcyclecounter);
    return b.CreateCall(f, {}, "clock");
}

// Ray query (SPV_KHR_ray_query) is Vulkan-only — only SpirvTarget overrides
// these. The defaults reject a RayQuery in a kernel lowered for a backend that
// has no ray-query support.
static cajeta::Exception rayQueryUnsupported(const char* backend) {
    return cajeta::Exception(
        "XPU kernel lowering: ray query (RayQuery / AccelerationStructure) is "
        "supported only on the Vulkan backend, not '" + std::string(backend) +
        "' (SPV_KHR_ray_query is a Vulkan-only extension)", "XPU-N02");
}

llvm::Type* LoweringTarget::rayQueryType(llvm::Module& /*m*/) {
    throw rayQueryUnsupported(name());
}

void LoweringTarget::rayQueryInitialize(
    llvm::IRBuilderBase& /*b*/, llvm::Module& /*m*/, llvm::Value* /*rqPtr*/,
    llvm::Value* /*asHandle*/, llvm::Value* /*rayFlags*/, llvm::Value* /*cullMask*/,
    llvm::Value* /*origin*/, llvm::Value* /*tMin*/, llvm::Value* /*direction*/,
    llvm::Value* /*tMax*/) {
    throw rayQueryUnsupported(name());
}

llvm::Value* LoweringTarget::rayQueryProceed(llvm::IRBuilderBase& /*b*/,
                                             llvm::Module& /*m*/,
                                             llvm::Value* /*rqPtr*/) {
    throw rayQueryUnsupported(name());
}

llvm::Value* LoweringTarget::rayQueryIntersectionType(
    llvm::IRBuilderBase& /*b*/, llvm::Module& /*m*/, llvm::Value* /*rqPtr*/,
    llvm::Value* /*intersection*/) {
    throw rayQueryUnsupported(name());
}

llvm::Value* LoweringTarget::rayQueryIntersectionPrimitiveIndex(
    llvm::IRBuilderBase& /*b*/, llvm::Module& /*m*/, llvm::Value* /*rqPtr*/,
    llvm::Value* /*intersection*/) {
    throw rayQueryUnsupported(name());
}

llvm::Value* LoweringTarget::rayQueryIntersectionT(
    llvm::IRBuilderBase& /*b*/, llvm::Module& /*m*/, llvm::Value* /*rqPtr*/,
    llvm::Value* /*intersection*/) {
    throw rayQueryUnsupported(name());
}

llvm::Value* LoweringTarget::rayQueryIntersectionBarycentrics(
    llvm::IRBuilderBase& /*b*/, llvm::Module& /*m*/, llvm::Value* /*rqPtr*/,
    llvm::Value* /*intersection*/) {
    throw rayQueryUnsupported(name());
}

llvm::Value* LoweringTarget::rayQueryIntersectionFrontFace(
    llvm::IRBuilderBase& /*b*/, llvm::Module& /*m*/, llvm::Value* /*rqPtr*/,
    llvm::Value* /*intersection*/) {
    throw rayQueryUnsupported(name());
}

void LoweringTarget::rayQueryConfirmIntersection(
    llvm::IRBuilderBase& /*b*/, llvm::Module& /*m*/, llvm::Value* /*rqPtr*/) {
    throw rayQueryUnsupported(name());
}

void LoweringTarget::rayQueryGenerateIntersection(
    llvm::IRBuilderBase& /*b*/, llvm::Module& /*m*/, llvm::Value* /*rqPtr*/,
    llvm::Value* /*tHit*/) {
    throw rayQueryUnsupported(name());
}

static cajeta::Exception coopMatrixUnsupported(const char* backend) {
    return cajeta::Exception(
        "XPU kernel lowering: cooperative matrix (CooperativeMatrix<T,...>) is "
        "supported only on the Vulkan backend, not '" + std::string(backend) +
        "' (SPV_KHR_cooperative_matrix is wired for the Vulkan flavor)",
        "XPU-N03");
}

llvm::Type* LoweringTarget::coopMatrixType(llvm::Module& /*m*/, llvm::Type* /*elem*/,
                                           uint32_t /*rows*/, uint32_t /*cols*/,
                                           uint32_t /*use*/) {
    throw coopMatrixUnsupported(name());
}

llvm::Value* LoweringTarget::coopMatrixLoad(
    llvm::IRBuilderBase& /*b*/, llvm::Module& /*m*/, llvm::Value* /*ptr*/,
    llvm::Value* /*layout*/, llvm::Value* /*stride*/, llvm::Type* /*matrixType*/,
    uint32_t /*rows*/, uint32_t /*cols*/, uint32_t /*use*/) {
    throw coopMatrixUnsupported(name());
}

void LoweringTarget::coopMatrixStore(
    llvm::IRBuilderBase& /*b*/, llvm::Module& /*m*/, llvm::Value* /*ptr*/,
    llvm::Value* /*matrixVal*/, llvm::Value* /*layout*/, llvm::Value* /*stride*/,
    uint32_t /*rows*/, uint32_t /*cols*/, uint32_t /*use*/) {
    throw coopMatrixUnsupported(name());
}

llvm::Value* LoweringTarget::coopMatrixMulAdd(
    llvm::IRBuilderBase& /*b*/, llvm::Module& /*m*/, llvm::Value* /*a*/,
    llvm::Value* /*bMat*/, llvm::Value* /*c*/, llvm::Type* /*matrixType*/) {
    throw coopMatrixUnsupported(name());
}

llvm::Value* LoweringTarget::coopMatrixSplat(
    llvm::IRBuilderBase& /*b*/, llvm::Module& /*m*/, llvm::Value* /*value*/,
    llvm::Type* /*matrixType*/) {
    throw coopMatrixUnsupported(name());
}

llvm::Type* LoweringTarget::bufferParamType(llvm::Module& m,
                                            llvm::Type* /*elemTy*/) {
    // NVPTX/AMDGPU: a buffer base is a global (addrspace 1) pointer — the same
    // type createKernel gives a kernel buffer param, so a helper arg matches it.
    return llvm::PointerType::get(m.getContext(), kGlobalAS);
}

llvm::Type* LoweringTarget::textureParamType(llvm::Module& m) {
    // Default (NVPTX emit-only): cudaTextureObject_t is a 64-bit handle by value.
    // AMDGPU overrides to ptr addrspace(4) (the HIP texture object).
    return llvm::Type::getInt64Ty(m.getContext());
}

// Admit the kernel parameters: Buffer<T>/arrays carry an element type,
// primitives a scalar type. This classification is backend-neutral; HOW the
// params become a signature is the backend's call (target.createKernel /
// materializeParam) — the Vulkan fork.
static std::vector<LoweringTarget::KernelParam> collectParams(
        const MethodPtr& method, llvm::LLVMContext& ctx) {
    std::vector<LoweringTarget::KernelParam> params;
    for (auto& p : method->getParameterList()) {
        if (!p) continue;
        if (p->getName() == "this") continue;
        CajetaTypePtr t = p->getType();
        if (isTextureType(t) || isTexture3DType(t) || isTexture1DType(t) ||
            isTexture2DArrayType(t) || isTextureCubeType(t)) {
            // Texture2D<T> / Texture3D<T> (Item 8): a sampled-image handle. `type`
            // is the texel scalar T, read off the type argument exactly like
            // Buffer<T> — float for the float/UNORM/half formats (the default
            // T = float32), i32 for the raw-integer formats (T = int32/uint32;
            // `isSigned` distinguishes them). The backend carries the handle itself
            // (a ptr on CPU, a descriptor on Vulkan). isTexture routes createKernel/
            // materializeParam and the `.sample()`/`.fetch()` lowering; the texel
            // type flows into the Vulkan image binding and fetchTexture's result;
            // textureDim (1, 2, or 3) selects the image dimensionality + coord arity.
            llvm::Type* texel = nullptr;
            bool texelSigned = true;
            if (auto cls = std::dynamic_pointer_cast<CajetaClass>(t)) {
                if (!cls->getTypeArguments().empty()) {
                    CajetaTypePtr arg0 = cls->getTypeArguments()[0];
                    texel = deviceScalarType(arg0, ctx);
                    texelSigned = typeIsSigned(arg0);
                }
            }
            if (!texel) texel = llvm::Type::getFloatTy(ctx);  // default texel (float32)
            LoweringTarget::KernelParam kp{p->getName(), /*isBuffer=*/false,
                                           texel, texelSigned,
                                           /*isTexture=*/true, /*isSampler=*/false};
            kp.textureDim = isTexture3DType(t) ? 3
                          : (isTexture1DType(t) ? 1
                          : (isTexture2DArrayType(t) ? 4
                          : (isTextureCubeType(t) ? 5 : 2)));
            params.push_back(kp);
        } else if (isImageType(t)) {
            // Image2D (writable images): the write twin of Texture2D — a 2-D
            // STORAGE_IMAGE handle. `type` is the texel scalar (f32); the backend
            // carries the handle (a descriptor on Vulkan). isImage routes
            // createKernel/materializeParam and the `.store()` lowering.
            params.push_back({p->getName(), /*isBuffer=*/false,
                              llvm::Type::getFloatTy(ctx), /*isSigned=*/true,
                              /*isTexture=*/false, /*isSampler=*/false,
                              /*isAccelStruct=*/false, /*isImage=*/true});
        } else if (isSamplerType(t)) {
            // Sampler (Item 8): filter/address descriptor. Carried by value as
            // the {i32 filterMode, i32 addressMode} struct (the same shape the
            // host marshaller packs) — NOT the by-value POD path, so it keeps a
            // distinct kind for the Vulkan descriptor fork. deviceStructInfo on
            // Sampler yields exactly {i32,i32}.
            DeviceStructInfo si = deviceStructInfo(t, ctx);
            llvm::Type* sty = si.type
                ? (llvm::Type*) si.type
                : (llvm::Type*) llvm::StructType::get(
                      ctx, {llvm::Type::getInt32Ty(ctx),
                            llvm::Type::getInt32Ty(ctx)});
            params.push_back({p->getName(), /*isBuffer=*/false, sty,
                              /*isSigned=*/false,
                              /*isTexture=*/false, /*isSampler=*/true});
        } else if (isAccelStructType(t)) {
            // AccelerationStructure (cajeta-gpu Part C): a descriptor-bound BVH.
            // The handle is opaque (an OpTypeAccelerationStructureKHR on Vulkan),
            // so `type` is an unused placeholder; materializeParam binds it and
            // the RayQuery ops consume the handle. isAccelStruct routes
            // createKernel/materializeParam, exactly like isTexture.
            params.push_back({p->getName(), /*isBuffer=*/false,
                              llvm::Type::getInt64Ty(ctx), /*isSigned=*/false,
                              /*isTexture=*/false, /*isSampler=*/false,
                              /*isAccelStruct=*/true});
        } else if (auto arr = std::dynamic_pointer_cast<CajetaArray>(t);
                   arr && isBufferType(arr->getElementType())) {
            // Buffer<T>[] — a bindless descriptor ARRAY of buffers (`bufs[idx][i]`).
            // The per-buffer element type T is read off the Buffer<T> element
            // exactly as the lone Buffer<T> case below; isBufferArray adds the
            // outer descriptor-array binding (one binding, descriptorCount = N).
            llvm::Type* elem = nullptr;
            bool elemSigned = true;
            if (auto cls = std::dynamic_pointer_cast<CajetaClass>(
                    arr->getElementType())) {
                if (!cls->getTypeArguments().empty()) {
                    CajetaTypePtr arg0 = cls->getTypeArguments()[0];
                    elem = deviceScalarType(arg0, ctx);
                    if (!elem) elem = deviceVectorType(arg0, ctx);
                    elemSigned = typeIsSigned(arg0);
                }
            }
            if (!elem) elem = llvm::Type::getFloatTy(ctx);
            LoweringTarget::KernelParam kp{p->getName(), /*isBuffer=*/true, elem,
                                           elemSigned};
            kp.isBufferArray = true;
            params.push_back(kp);
        } else if (isBufferType(t)) {
            llvm::Type* elem = nullptr;
            bool elemSigned = true;
            if (auto cls = std::dynamic_pointer_cast<CajetaClass>(t)) {
                if (!cls->getTypeArguments().empty()) {
                    CajetaTypePtr arg0 = cls->getTypeArguments()[0];
                    elem = deviceScalarType(arg0, ctx);
                    if (!elem) elem = deviceVectorType(arg0, ctx);  // Buffer<Vector<..>>
                    elemSigned = typeIsSigned(arg0);
                }
            }
            if (!elem) elem = llvm::Type::getFloatTy(ctx);  // default element
            params.push_back({p->getName(), /*isBuffer=*/true, elem, elemSigned});
        } else if (llvm::Type* st = deviceScalarType(t, ctx)) {
            params.push_back({p->getName(), /*isBuffer=*/false, st,
                              typeIsSigned(t)});
        } else if (llvm::Type* mt = deviceMatrixType(t, ctx)) {
            // Matrix<T,R,C> by value (B1 follow-on): a flat <R*C x T> aggregate
            // kernel param. The host packs R*C contiguous elements (the scalar/
            // else marshalling path), and the body recovers (R,C) from
            // matrixShapes (set in lowerBody) so m[r][c]/matmul/methods work.
            // Rides the non-buffer by-value path: createKernel emits it by value
            // (NVPTX/AMDGPU/CPU), Vulkan binds a single-element SSBO.
            params.push_back({p->getName(), /*isBuffer=*/false, mt,
                              /*isSigned=*/false});
        } else if (llvm::Type* vt = deviceVectorType(t, ctx)) {
            // Vector<T,N> by value: same intrinsic-value-type by-value path as
            // Matrix (the slot is the <N x T> vector; vectorSlotType recovers it
            // for .x/[i]/dot/length). Closes the same gap for Vector.
            params.push_back({p->getName(), /*isBuffer=*/false, vt,
                              /*isSigned=*/false});
        } else if (DeviceStructInfo si = deviceStructInfo(t, ctx); si.type) {
            // POD struct by value (Item 7) — an aggregate kernel param. It rides
            // the non-buffer path: createKernel emits it by value, lowerBody
            // gives it an entry-alloca slot, and field reads GEP into that slot.
            params.push_back({p->getName(), /*isBuffer=*/false, si.type,
                              /*isSigned=*/false});
        } else {
            unsupported("kernel parameter type '" +
                        (t ? t->toCanonical() : std::string("?")) + "'");
        }
    }
    return params;
}

std::vector<KernelParamInfo> collectKernelParamInfo(const MethodPtr& method,
                                                    llvm::LLVMContext& ctx,
                                                    const llvm::DataLayout& dl) {
    std::vector<KernelParamInfo> info;
    if (!method) return info;
    for (auto& p : collectParams(method, ctx)) {
        uint8_t kind = KernelParamInfo::Scalar;
        unsigned bytes = 0;
        if (p.isTexture) {
            kind = KernelParamInfo::Texture;
        } else if (p.isImage) {
            kind = KernelParamInfo::Image;
        } else if (p.isSampler) {
            kind = KernelParamInfo::Sampler;
        } else if (p.isAccelStruct) {
            // The AccelStruct binding encodes the native impl (accelImpl() ==
            // VulkanNative): the noun is bound as an acceleration-structure
            // descriptor. The software-BVH impl binds the AS as a plain buffer
            // base instead — but that only occurs on CPU (which never reaches this
            // Vulkan kparams/launch path), so impl == backend makes this correct
            // today. Selecting the kind per-impl is the heuristic brick's change
            // (when one backend can build either); the launch asserts the match.
            kind = KernelParamInfo::AccelStruct;
        } else if (p.isBufferArray) {
            kind = KernelParamInfo::BufferArray;   // checked before isBuffer (both true)
        } else if (p.isBuffer) {
            kind = KernelParamInfo::Buffer;
        } else if (p.type) {
            // POD struct: the marshalled by-value footprint under the HOST
            // module's real DataLayout — must match how the launch site packs the
            // argv (an empty DataLayout under-sizes e.g. {i32,i64} to 12 vs 16, so
            // the runtime memcpy'd too few bytes and the device read past the
            // SSBO). Scalars: their byte width.
            // Aggregate by-value params (POD struct, or a Matrix/Vector flat
            // <N x T>) use the real alloc size; getScalarSizeInBits() on a
            // vector returns the ELEMENT width, undersizing the marshalled arg.
            bytes = (p.type->isStructTy() || p.type->isVectorTy())
                ? (unsigned) dl.getTypeAllocSize(p.type)
                : (p.type->getScalarSizeInBits() + 7u) / 8u;
        }
        info.push_back({kind, bytes});
    }
    return info;
}

// Base default for wave rotate: the width-agnostic shuffle form, built on the
// other wave seams so NVPTX/AMD/CPU get rotate for free. Vulkan overrides to the
// native OpGroupNonUniformRotateKHR. Out-of-line because LoweringTarget.h only
// forward-declares IRBuilderBase. Keep the (laneId+delta) mod width semantics in
// lock-step with the doc and the native op (the device test cross-checks both).
llvm::Value* LoweringTarget::waveRotate(llvm::IRBuilderBase& b, llvm::Module& m,
                                        llvm::Value* value, llvm::Value* delta) {
    llvm::Value* lane = waveLaneId(b, m);
    llvm::Value* width = waveWidth(b, m);
    llvm::Value* src = b.CreateURem(
        b.CreateAdd(lane, delta), width, "wave.rotate.src");
    return waveShuffle(b, m, value, src);
}

// Base default for the exclusive prefix scan: a width-agnostic Hillis-Steele
// scan over the existing wave seams, so NVPTX (and AMDGPU, once it overrides
// waveShuffleDivergent → ds_bpermute) get the scan without a native op. Vulkan
// overrides to OpGroupNonUniform ExclusiveScan; CPU to a VFABI variant.
// Out-of-line because LoweringTarget.h only forward-declares IRBuilderBase.
llvm::Value* LoweringTarget::waveScan(llvm::IRBuilderBase& b, llvm::Module& m,
                                      WaveScanOp op, llvm::Value* value) {
    llvm::LLVMContext& ctx = m.getContext();
    llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Value* lane = waveLaneId(b, m);
    llvm::Value* width = waveWidth(b, m);
    auto combine = [&](llvm::Value* x, llvm::Value* y) {
        return op == WaveScanOp::Sum ? b.CreateAdd(x, y) : b.CreateMul(x, y);
    };
    // Inclusive Hillis-Steele: acc[i] op= acc[i-d] for d = 1,2,4,… < width.
    // The loop trip count is log2(width); width folds to a constant on the
    // backends that take this path (NVPTX warp size, CPU rewritten width).
    llvm::Function* fn = b.GetInsertBlock()->getParent();
    llvm::BasicBlock* preheader = b.GetInsertBlock();
    llvm::BasicBlock* loop = llvm::BasicBlock::Create(ctx, "scan.loop", fn);
    llvm::BasicBlock* done = llvm::BasicBlock::Create(ctx, "scan.done", fn);
    b.CreateBr(loop);
    b.SetInsertPoint(loop);
    llvm::PHINode* accPhi = b.CreatePHI(i32, 2, "scan.acc");
    llvm::PHINode* dPhi = b.CreatePHI(i32, 2, "scan.d");
    accPhi->addIncoming(value, preheader);
    dPhi->addIncoming(llvm::ConstantInt::get(i32, 1), preheader);
    // pred = lane >= d; read acc from lane-d (clamped to a valid lane when
    // pred is false — the result is discarded by the select).
    llvm::Value* pred = b.CreateICmpUGE(lane, dPhi);
    llvm::Value* srcRaw = b.CreateSub(lane, dPhi);
    llvm::Value* src = b.CreateSelect(pred, srcRaw, lane);
    llvm::Value* other = waveShuffleDivergent(b, m, accPhi, src);
    llvm::Value* acc = b.CreateSelect(pred, combine(accPhi, other), accPhi);
    llvm::Value* dNext = b.CreateShl(dPhi, 1);
    accPhi->addIncoming(acc, loop);
    dPhi->addIncoming(dNext, loop);
    b.CreateCondBr(b.CreateICmpULT(dNext, width), loop, done);
    b.SetInsertPoint(done);
    llvm::PHINode* inc = b.CreatePHI(i32, 1, "scan.inclusive");
    inc->addIncoming(acc, loop);
    // Exclusive = inclusive shifted up one lane: exc[i] = inc[i-1], exc[0] = id.
    llvm::Value* identity =
        llvm::ConstantInt::get(i32, op == WaveScanOp::Sum ? 0 : 1);
    llvm::Value* isFirst = b.CreateICmpEQ(
        lane, llvm::ConstantInt::get(i32, 0));
    llvm::Value* prevLane = b.CreateSelect(
        isFirst, lane, b.CreateSub(lane, llvm::ConstantInt::get(i32, 1)));
    llvm::Value* prev = waveShuffleDivergent(b, m, inc, prevLane);
    return b.CreateSelect(isFirst, identity, prev);
}

// Quad (2x2) op defaults — width-agnostic forms built on the wave seams (see
// LoweringTarget.h). Out-of-line because the header only forward-declares
// IRBuilderBase. NVPTX/AMDGPU/CPU take these; Vulkan overrides to native ops.
// A quad is the four lanes [laneId & ~3 .. +3].
llvm::Value* LoweringTarget::quadBroadcast(llvm::IRBuilderBase& b,
                                           llvm::Module& m, llvm::Value* value,
                                           llvm::Value* index) {
    // Read from lane (laneId & ~3) + index — the `index`-th lane of this quad.
    llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
    llvm::Value* lane = waveLaneId(b, m);
    llvm::Value* quadBase =
        b.CreateAnd(lane, llvm::ConstantInt::get(i32, ~3u), "quad.base");
    llvm::Value* src = b.CreateAdd(quadBase, index, "quad.bcast.src");
    return waveShuffleDivergent(b, m, value, src);
}

llvm::Value* LoweringTarget::quadSwap(llvm::IRBuilderBase& b, llvm::Module& m,
                                      llvm::Value* value, unsigned direction) {
    // Partner = laneId ^ (direction+1): horiz ^1, vert ^2, diag ^3.
    llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
    llvm::Value* lane = waveLaneId(b, m);
    llvm::Value* src = b.CreateXor(
        lane, llvm::ConstantInt::get(i32, direction + 1), "quad.swap.src");
    return waveShuffleDivergent(b, m, value, src);
}

// The four vote bits of this lane's quad: (ballot(pred) >> (laneId & ~3)) & 0xF.
// all = nibble == 0xF; any = nibble != 0. Assumes full quads (inactive lanes
// ballot 0, matching the native RequireFullQuadsKHR semantics).
static llvm::Value* quadNibble(LoweringTarget& t, llvm::IRBuilderBase& b,
                               llvm::Module& m, llvm::Value* pred) {
    llvm::LLVMContext& ctx = m.getContext();
    llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
    llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Value* mask = t.waveBallot(b, m, pred);   // i64
    llvm::Value* lane = t.waveLaneId(b, m);         // i32
    llvm::Value* quadBase =
        b.CreateAnd(lane, llvm::ConstantInt::get(i32, ~3u));
    llvm::Value* shifted = b.CreateLShr(mask, b.CreateZExt(quadBase, i64));
    return b.CreateAnd(shifted, llvm::ConstantInt::get(i64, 0xF), "quad.nibble");
}

llvm::Value* LoweringTarget::quadAll(llvm::IRBuilderBase& b, llvm::Module& m,
                                     llvm::Value* pred) {
    llvm::Value* nib = quadNibble(*this, b, m, pred);
    return b.CreateICmpEQ(nib, llvm::ConstantInt::get(nib->getType(), 0xF),
                          "quad.all");
}

llvm::Value* LoweringTarget::quadAny(llvm::IRBuilderBase& b, llvm::Module& m,
                                     llvm::Value* pred) {
    llvm::Value* nib = quadNibble(*this, b, m, pred);
    return b.CreateICmpNE(nib, llvm::ConstantInt::get(nib->getType(), 0),
                          "quad.any");
}

llvm::Function* lowerKernel(const MethodPtr& method, llvm::Module& deviceModule,
                            LoweringTarget& target, const std::string& entryName) {
    if (!method) unsupported("null kernel method");
    llvm::LLVMContext& ctx = deviceModule.getContext();

    std::vector<LoweringTarget::KernelParam> params = collectParams(method, ctx);

    std::string kname = entryName.empty() ? method->getName() : entryName;
    llvm::Function* fn = target.createKernel(deviceModule, kname, params);

    DeviceLowerer lowerer(deviceModule, fn, target);
    lowerer.setParams(std::move(params));
    // A per-kernel cache of lowered @Device helper functions (shared with any
    // nested helper lowering), so each helper is emitted once and recursion is
    // caught. The functions live in the module; the cache is just for dedup.
    DeviceLowerer::DeviceFnCache deviceFns;
    lowerer.setDeviceContext(method->getParent(), &deviceFns);
    lowerer.lowerBody(method);
    // If the kernel used a cross-lane subgroup op, ask the backend to request
    // maximal reconvergence (a no-op on backends that don't model it). This is
    // the correctness companion to Wave.shuffle/ballot/reduce.
    if (lowerer.usedSubgroupOp())
        target.onSubgroupOpsUsed(fn, deviceModule);
    return fn;
}

} // namespace xpu
} // namespace cajeta
