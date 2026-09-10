// Shared @Kernel AST -> device llvm::Function lowering; see KernelLowering.h.

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
#include "../../type/CajetaFunctionType.h"
#include "../../type/CajetaConstantType.h"
#include "../../type/VectorOps.h"
#include "../../type/MatrixOps.h"
#include "../../type/QuaternionOps.h"
#include "../core/XpuAttributes.h"
#include "../core/XpuKernelAttr.h"
#include "../core/KernelArgTrait.h"
#include "../core/KernelAccess.h"
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
#include "../../asn/expression/CallExpression.h"
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

// Global thread index for dimension `dim`: workgroupId * workgroupDim +
// threadId. The identity holds on NVPTX and AMDGPU alike, so it lives in the
// shared base; a backend with a native global-id intrinsic overrides it.
llvm::Value* LoweringTarget::globalId(llvm::IRBuilderBase& b, llvm::Module& m,
                                      unsigned dim) {
    llvm::Value* wid = workgroupId(b, m, dim);
    llvm::Value* wdim = workgroupDim(b, m, dim);
    llvm::Value* tid = threadId(b, m, dim);
    return b.CreateAdd(b.CreateMul(wid, wdim), tid, "gid");
}

// Apply the CAJETA_GPU_<FEATURE>_IMPL override to a per-backend base tier:
// "software" gives Portable, "native" gives Native, anything else keeps `base`.
// The compile-time twin of caj_resolve_as_impl; read here and only here.
LoweringTarget::ImplTier resolveImplTier(const char* feature,
                                         LoweringTarget::ImplTier base) {
    std::string var = std::string("CAJETA_GPU_") + feature + "_IMPL";
    const char* env = std::getenv(var.c_str());
    if (env && *env) {
        if (std::string(env) == "software") return LoweringTarget::ImplTier::Portable;
        if (std::string(env) == "native")   return LoweringTarget::ImplTier::Native;
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
        "XPU kernel lowering: unsupported construct Ã¢ÂÂ " + what,
        "XPU-N01");
}

// addrspace(1) is device global memory and addrspace(3) workgroup/shared;
// NVPTX and AMDGPU agree on both, so these are shared, not a fork point.
constexpr unsigned kGlobalAS = 1;
constexpr unsigned kSharedAS = 3;

// Map a primitive CajetaType to a device LLVM scalar type, built fresh in the
// device context (NOT CajetaType::getLlvmType(), whose cache is bound to the
// host context). Returns nullptr for non-primitives.
llvm::Type* deviceScalarType(const CajetaTypePtr& t, llvm::LLVMContext& ctx) {
    if (!t) return nullptr;
    CajetaTypeFlags f = t->getTypeFlags();
    if (!(f & PRIMITIVE_FLAG)) return nullptr;
    if (f & FLOAT_FLAG) {
        if (f & BIT_64_FLAG) return llvm::Type::getDoubleTy(ctx);
        if (f & BIT_16_FLAG) {
            // The type-ID byte lives in bits 32-39, so the mask must be 64-bit:
            // `unsigned long` is 32-bit on Windows (LLP64) and truncates it to
            // 0, misclassifying bfloat16 as half.
            constexpr CajetaTypeFlags kIdMask = 0x000000FF00000000ULL;
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

// Map a Vector<T,N> to a device LLVM `<N x T>`, built fresh in the device
// context; nullptr when `t` is not a CajetaVector of a device scalar.
llvm::Type* deviceVectorType(const CajetaTypePtr& t, llvm::LLVMContext& ctx) {
    auto vec = std::dynamic_pointer_cast<CajetaVector>(t);
    if (!vec) return nullptr;
    llvm::Type* elem = deviceScalarType(vec->getElementType(), ctx);
    if (!elem) return nullptr;
    // FixedVectorType::get(elem, 0) asserts in a debug LLVM; reject N == 0.
    if (vec->getLanes() == 0)
        throw cajeta::Exception(
            "XPU kernel lowering: Vector<T, 0> has no lanes Ã¢ÂÂ N must be > 0",
            "XPU-N01");
    return llvm::FixedVectorType::get(elem, vec->getLanes());
}

// Map a Matrix<T,R,C> to a device LLVM `<R*C x T>` (flat row-major), built
// fresh in the device context; nullptr when `t` is not a CajetaMatrix.
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

// Map a Quaternion<T> to a device LLVM `<4 x T>` (w, x, y, z), or nullptr when
// `t` is not a CajetaQuaternion. The slot type is shared with Vector<T,4>.
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
    static const std::string kPrefix = "cajeta.xpu.KernelBuffer";
    return c.compare(0, kPrefix.size(), kPrefix) == 0;
}

bool typeIsSigned(const CajetaTypePtr& t) {
    return t && (t->getTypeFlags() & SIGNED_FLAG);
}

// Device layout of a POD struct kernel param: its primitive fields in
// declaration order, with the host vtable word at slot 0 stripped (as the
// launch-site marshaller does). `type` is null when `t` is not a POD struct.
struct DeviceStructInfo {
    llvm::StructType* type = nullptr;
    // `sub` is non-empty only for a nested @ValueType field: its own field map,
    // so `param.vfield.subfield` resolves to a multi-index extractvalue.
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
        // Vector/Matrix fields carry PRIMITIVE_FLAG but not VALUE_TYPE_FLAG, so
        // they must match here, ahead of the nested-@ValueType branch below.
        if (llvm::Type* vty = deviceVectorType(pt, ctx)) {
            info.fields[prop->getName()] = {idx, vty, false, {}};
            ftys.push_back(vty);
            ++idx;
            continue;
        }
        if (llvm::Type* mty = deviceMatrixType(pt, ctx)) {
            info.fields[prop->getName()] = {idx, mty, false, {}};
            ftys.push_back(mty);
            ++idx;
            continue;
        }
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

// Coordinate components a texture kind's sample/fetch takes, keyed by the
// KernelParam::textureDim code (1=1D, 2=2D, 3=3D, 4=2D-array, 5=cube): the
// linear kinds have arity = dim, 2D-array and cube are both 3.
static inline int textureCoordArity(int dim) {
    return dim <= 3 ? dim : 3;
}

// One device kernel's worth of lowering state.
class DeviceLowerer {
public:
    using DeviceFnCache = std::map<const Method*, llvm::Function*>;

    DeviceLowerer(llvm::Module& m, llvm::Function* fn, LoweringTarget& target)
        : mod(m), ctx(m.getContext()), builder(ctx), fn(fn), target(target) {}

    // @Device helper-call context: `c` resolves a bare helper name to a sibling
    // method; `cache` holds the already-lowered @Device functions (a null entry
    // means "being lowered" - a recursive call, which is rejected).
    void setDeviceContext(std::shared_ptr<CajetaClass> c, DeviceFnCache* cache) {
        cls = std::move(c);
        deviceFns = cache;
    }

    // Record the admitted kernel params; lowerBody materializes them into the
    // entry block, which has to exist first.
    void setParams(std::vector<LoweringTarget::KernelParam> p) {
        kparams = std::move(p);
    }

    // Mark this a @Device helper: its params are ordinary LLVM arguments, so
    // lowerBody reads fn->getArg(idx) rather than binding a fresh descriptor.
    void setParamsAsArgs(bool b) { paramsAsArgs = b; }

    // True iff the lowered body used a cross-lane subgroup op (shuffle/ballot/
    // reduce). Drives the maximal-reconvergence request at finalization.
    bool usedSubgroupOp() const { return usedSubgroupOp_; }

    void lowerBody(const MethodPtr& method) {
        if (!cls) cls = method->getParent();   // for @Device helper resolution
        builder.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", fn));
        if (method && isFastMath(*method)) {
            llvm::FastMathFlags fmf;
            fmf.setFast();   // contract + reassoc + arcp + afn + nnan/ninf/nsz
            builder.setFastMathFlags(fmf);
        }
        unsigned idx = 0;
        for (auto& p : kparams) {
            if (p.isBufferArray) {
                bufferArrayBindings[p.name] = {idx, p.type, p.isSigned};
                // `paramsAsArgs` is the @Device-helper flag, NOT the Vulkan
                // distinction: descriptor-bound backends have no prologue base.
                bufferArrayBases[p.name] =
                    target.descriptorBoundParams()
                        ? nullptr
                        : (paramsAsArgs
                               ? fn->getArg(idx)
                               : target.materializeParam(builder, mod, fn, idx, p));
                ++idx;
                continue;
            }
            llvm::Value* v = paramsAsArgs
                ? fn->getArg(idx)
                : target.materializeParam(builder, mod, fn, idx, p);
            ++idx;
            if (p.isAccelStruct) {
                accelHandles[p.name] = v;
            } else if (p.isTexture) {
                textureHandles[p.name] = v;
                textureTexelTypes[p.name] = p.type;
                textureDims[p.name] = p.textureDim;
            } else if (p.isImage) {
                imageHandles[p.name] = v;
            } else if (p.isSampler) {
                samplerHandles[p.name] = v;
            } else if (p.isBuffer) {
                bufferBases[p.name] = v;
                bufferElems[p.name] = p.type;
                bufferElemSigned[p.name] = p.isSigned;
            } else if (p.type->isStructTy()) {
                // Keep the aggregate an SSA value: under SPIR-V logical
                // addressing an aggregate store to a Function-storage pointer is
                // rejected by spirv-val ("not a logical pointer").
                structValues[p.name] = v;
            } else {
                llvm::Value* slot = entryAlloca(p.type, p.name);
                builder.CreateStore(v, slot);
                values[p.name] = slot;
                slotTypes[p.name] = p.type;
                signedness[p.name] = p.isSigned;
            }
        }
        for (auto& p : method->getParameterList()) {
            if (!p || p->getName() == "this") continue;
            DeviceStructInfo si = deviceStructInfo(p->getType(), ctx);
            if (si.type) structFields[p->getName()] = std::move(si);
            if (auto mt = std::dynamic_pointer_cast<CajetaMatrix>(p->getType()))
                matrixShapes[p->getName()] = {mt->getRows(), mt->getCols()};
            if (p->getType() && p->getType()->isValueType())
                valueTypeNames[p->getName()] = p->getType();
        }
        auto registerCtor = [&](const std::shared_ptr<CajetaClass>& c) {
            if (c && c->isValueType())
                valueTypeCtors[c->getQName()->getTypeName()] = c;
        };
        for (auto& [n, t] : valueTypeNames)
            registerCtor(std::dynamic_pointer_cast<CajetaClass>(t));
        registerCtor(method->getParent());
        inferTileUses(method->getBlock());
        scanCoopMatrixTiers(method->getBlock());
        lowerStatement(method->getBlock());
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
    // Scalar locals/params live in mutable entry-block allocas so loops and
    // reassignment work; buffer bases are never reassigned and are held direct.
    std::map<std::string, llvm::Value*> values;       // scalar name -> alloca slot
    std::map<std::string, llvm::Type*> slotTypes;     // scalar name -> slot elem type
    std::map<std::string, bool> signedness;
    std::map<std::string, llvm::Value*> bufferBases;  // buffer name -> addrspace(1) ptr
    std::map<std::string, llvm::Type*> bufferElems;   // buffer name -> element type
    // Bindless buffer-array params (Buffer<T>[]): binding + element type by
    // name. There is no single base - `bufs[idx]` selects a descriptor.
    struct BufferArrayInfo { unsigned binding; llvm::Type* elemTy; bool isSigned; };
    std::map<std::string, BufferArrayInfo> bufferArrayBindings;
    std::map<std::string, llvm::Value*> bufferArrayBases;  // CPU handle-array ptr
    // Texture2D / Sampler params: the materialized backend handle by name;
    // `tex.sample(s, u, v)` hands both to target.sampleTexture.
    std::map<std::string, llvm::Value*> textureHandles;  // texture name -> handle
    std::map<std::string, llvm::Type*> textureTexelTypes; // name -> texel scalar
    std::map<std::string, int> textureDims;               // texture name -> 2 or 3
    std::map<std::string, llvm::Value*> imageHandles;    // image name -> storage-image handle
    std::map<std::string, llvm::Value*> samplerHandles;  // sampler name -> descriptor
    // AccelerationStructure params and RayQuery locals: the AS descriptor handle
    // by name, and the RayQuery OpVariable Function alloca by name.
    std::map<std::string, llvm::Value*> accelHandles;    // AS name -> descriptor
    std::map<std::string, llvm::Value*> rayQuerySlots;   // RayQuery name -> alloca
    // RayQuery alloca -> the software BVH handle recorded at rq.initialize, so
    // each rq.proceed() can pass it to the SoftwareRayQuery walk.
    std::map<llvm::Value*, llvm::Value*> rayQueryBvh;
    bool swCursorCached = false;
    DeviceStructInfo swCursorInfoCache;
    // CooperativeMatrix locals: the alloca holds the tile - the opaque
    // OpTypeCooperativeMatrixKHR on the native tier, a flat array on software.
    struct CoopMatrixSlot {
        llvm::Value* alloca = nullptr;
        llvm::Type* matrixType = nullptr;   // opaque tile (native) or [N x elem] (software)
        bool software = false;
        llvm::Type* elemType = nullptr;     // device scalar (storage) element type
        bool elemSigned = true;             // the only carrier: LLVM ints are signless
        uint32_t rows = 0, cols = 0, use = 0;
    };
    std::map<std::string, CoopMatrixSlot> coopMatrixSlots;
    // Tile<T,Rows,Cols>: the SPIR-V "Use" (A=0 / B=1 / accumulator=2) is hidden
    // from the author and inferred by inferTileUses() before slot construction.
    std::map<std::string, uint32_t> tileInferredUse;
    // Set by scanCoopMatrixTiers when this kernel's tiles straddle tiers. A tier
    // belongs to the GEMM, so a straddling kernel demotes every tile to Portable.
    bool coopStraddleDemote = false;
    // (dtype,shape) keys already announced, so the mma-tiering note fires once.
    std::set<std::string> notedCoopTiers;

    std::vector<LoweringTarget::KernelParam> kparams;  // admitted params
    bool paramsAsArgs = false;  // true for @Device helpers (params are fn args)
    std::map<std::string, bool> bufferElemSigned;  // buffer name -> elem signed?
    // Set when the body lowers a cross-lane subgroup op; read at finalization to
    // request maximal reconvergence where the backend models it.
    bool usedSubgroupOp_ = false;
    // POD struct params: the materialized aggregate SSA value by name plus its
    // field map. Field reads are extractvalue, never an alloca (SPIR-V logical).
    std::map<std::string, llvm::Value*> structValues;       // name -> struct value
    std::map<std::string, DeviceStructInfo> structFields;   // name -> field map
    // @ValueType-typed names -> their CajetaType. Kernel bodies are not
    // host-type-resolved, so this is how `a OP b` recovers an operand's class.
    std::map<std::string, CajetaTypePtr> valueTypeNames;
    // @ValueType classes constructible in this body, keyed by simple type name:
    // an aggregate-returning @Device operator needs the layout by that name.
    std::map<std::string, std::shared_ptr<CajetaClass>> valueTypeCtors;
    // Matrix<T,R,C> locals: name -> (rows, cols). A matrix and a Vector<R*C>
    // share one slot type, so a name absent here is NOT a matrix.
    std::map<std::string, std::pair<unsigned, unsigned>> matrixShapes;
    // Quaternion local/param names: a quaternion shares the `<4 x T>` slot with
    // Vector<T,4>, so membership here routes `*` and the methods to quaternions.
    std::set<std::string> quaternionLocals;
    std::shared_ptr<CajetaClass> cls;              // declaring class (helper resolution)
    DeviceFnCache* deviceFns = nullptr;            // shared @Device function cache
    // A dynamic shared array kept typed (Vulkan): name -> {global, array type}.
    // Indexed as gep(arrTy, gv, {0, i}) so the OpTypeArray survives.
    std::map<std::string, std::pair<llvm::Value*, llvm::Type*>> arrayShared;
    // Swizzled<T,S> tiles: LDS base pointer -> row stride S. Every access runs
    // through target.swizzleAddr(idx, S), on read and write alike.
    std::map<llvm::Value*, uint32_t> swizzledBaseStride;
    // BlockPadded<T,Block,Pad> tiles: base -> {block period, pad} in elements.
    // Addressing runs through target.blockPadAddr on read and write alike.
    std::map<llvm::Value*, std::pair<uint32_t, uint32_t>> blockPadOfBase;
    // At most one dynamic (runtime-sized) shared array per kernel: the extern
    // unsized addrspace(3) region is a single base and two would alias.
    bool emittedDynamicShared = false;

    // Bounded device-side dispatch: a function-typed device local is an i32 tag
    // over a closed @Device-static candidate set, not a pointer (SPIR-V has none).
    struct DeviceCallable {
        CajetaTypePtr sig;                  // the CajetaFunctionType (params/return)
        std::vector<MethodPtr> candidates;  // ordered; vector index == dispatch tag
        bool isTable = false;               // table: tag is the call-site index expr
        llvm::Value* tagVal = nullptr;      // variable form: the i32 tag (const/select)
    };
    std::map<std::string, DeviceCallable> callables;  // local name -> callable

    // continue/break targets for the innermost enclosing loop.
    struct LoopTarget {
        llvm::BasicBlock* continueBB;
        llvm::BasicBlock* breakBB;
        std::string label;            // "" for an unlabeled loop
    };
    std::vector<LoopTarget> loopTargets;
    std::string pendingLoopLabel_;    // set by an IdentifierLabel; the next loop consumes it

    // Push a loop's break/continue targets, attaching any pending label (from a
    // preceding `label:`), then clear it so it binds to exactly one loop.
    void pushLoop(llvm::BasicBlock* continueBB, llvm::BasicBlock* breakBB) {
        loopTargets.push_back({continueBB, breakBB, pendingLoopLabel_});
        pendingLoopLabel_.clear();
    }
    // Resolve a break/continue target: a named label walks outward for a match,
    // an empty label is the innermost loop; null when there is no such target.
    const LoopTarget* findLoopTarget(const std::string& label) {
        if (label.empty())
            return loopTargets.empty() ? nullptr : &loopTargets.back();
        for (auto it = loopTargets.rbegin(); it != loopTargets.rend(); ++it)
            if (it->label == label) return &*it;
        return nullptr;
    }

    // Decode a kernel string literal (raw text including its quotes) into a
    // C-string constant. `%d`/`%f` are not escapes and pass straight through.
    static std::string decodeKernelString(const std::string& raw) {
        std::string s = raw;
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
            s = s.substr(1, s.size() - 2);
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                switch (s[++i]) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '\\': out += '\\'; break;
                    case '"': out += '"'; break;
                    case '0': out += '\0'; break;
                    default: out += s[i]; break;
                }
            } else {
                out += s[i];
            }
        }
        return out;
    }

    // Allocate a slot in the function entry block so it dominates every use. The
    // alloca address space is the backend's (0 NVPTX, 5/private AMDGPU).
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
        if (auto il = std::dynamic_pointer_cast<IdentifierLabel>(node)) {
            pendingLoopLabel_ = il->getIdentifier();
            lowerStatement(il->getBody());
            pendingLoopLabel_.clear();
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
            const LoopTarget* t = findLoopTarget(bs->getLabel());
            if (!t) unsupported(bs->getLabel().empty()
                    ? "break outside loop"
                    : "break: no enclosing loop labeled '" + bs->getLabel() + "'");
            builder.CreateBr(t->breakBB);
            // Dead trailing statements still need a block to land in; the
            // enclosing loop's tail fixup terminates it.
            builder.SetInsertPoint(llvm::BasicBlock::Create(ctx, "after.break", fn));
            return;
        }
        if (auto cs = std::dynamic_pointer_cast<ContinueStatement>(node)) {
            const LoopTarget* t = findLoopTarget(cs->getLabel());
            if (!t) unsupported(cs->getLabel().empty()
                    ? "continue outside loop"
                    : "continue: no enclosing loop labeled '" + cs->getLabel() + "'");
            builder.CreateBr(t->continueBB);
            builder.SetInsertPoint(llvm::BasicBlock::Create(ctx, "after.continue", fn));
            return;
        }
        if (auto es = std::dynamic_pointer_cast<ExpressionStatement>(node)) {
            lowerExprStatement(es->getExpression());
            return;
        }
        if (auto rs = std::dynamic_pointer_cast<ReturnStatement>(node)) {
            if (rs->getExpression() && target.shaderOutputReturn()) {
                llvm::Value* v = lowerExpr(rs->getExpression());
                target.storeShaderOutput(builder, mod, fn, v);
                builder.CreateRetVoid();
            } else if (fn->getReturnType()->isVoidTy() || !rs->getExpression()) {
                builder.CreateRetVoid();
            } else {
                llvm::Value* v = lowerExpr(rs->getExpression());
                builder.CreateRet(coerceTo(v, fn->getReturnType(),
                                           exprSigned(rs->getExpression())));
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
            // RayQuery local: the alloca IS the object (an OpVariable Function),
            // so any `stack RayQuery()` initializer carries no value to store.
            if (isRayQueryType(declType)) {
                llvm::Type* rqTy = target.softwareRayQuery()
                    ? (llvm::Type*) swCursorInfo().type
                    : target.rayQueryType(mod);
                if (!rqTy)
                    unsupported("software RayQuery needs cajeta.xpu.SwRayCursor");
                rayQuerySlots[nm] = entryAlloca(rqTy, nm);
                continue;
            }
            if (isCooperativeMatrixType(declType) || isTileType(declType)) {
                coopMatrixSlots[nm] = buildCoopMatrixSlot(declType, nm);
                continue;
            }
            if (declType && declType->isValueType()) {
                DeviceStructInfo si = deviceStructInfo(declType, ctx);
                if (si.type) {
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
            if (auto fnT = std::dynamic_pointer_cast<CajetaFunctionType>(declType)) {
                lowerCallableDecl(nm, fnT, vd->getInitializer());
                continue;
            }
            if (auto arrT = std::dynamic_pointer_cast<CajetaArray>(declType)) {
                if (auto efn = std::dynamic_pointer_cast<CajetaFunctionType>(
                        arrT->getElementType())) {
                    lowerCallableDecl(nm, efn, vd->getInitializer());
                    continue;
                }
            }
            llvm::Type* slotTy = deviceScalarType(declType, ctx);
            if (!slotTy) slotTy = deviceVectorType(declType, ctx);  // Vector<T,N>
            if (!slotTy) {
                if (auto matT = std::dynamic_pointer_cast<CajetaMatrix>(declType)) {
                    slotTy = deviceMatrixType(declType, ctx);
                    matrixShapes[nm] = {matT->getRows(), matT->getCols()};
                }
            }
            if (!slotTy) {
                if (std::dynamic_pointer_cast<CajetaQuaternion>(declType)) {
                    slotTy = deviceQuaternionType(declType, ctx);
                    quaternionLocals.insert(nm);
                }
            }
            auto init = vd->getInitializer();
            if (!init || init->getChildren().empty()) {
                if (!slotTy) unsupported("uninitialized local of non-scalar type");
                values[nm] = entryAlloca(slotTy, nm);
                slotTypes[nm] = slotTy;
                signedness[nm] = typeIsSigned(declType);
                continue;
            }
            auto initExpr = std::dynamic_pointer_cast<Expression>(
                init->getChildren()[0]);
            if (auto ne = std::dynamic_pointer_cast<NewExpression>(initExpr)) {
                if (ne->getSharedAlloc()) {
                    lowerSharedDecl(nm, declType, ne);
                    continue;
                }
            }
            if (auto lit =
                    std::dynamic_pointer_cast<ArrayLiteralExpression>(initExpr)) {
                if (lit->isSharedAlloc()) {
                    lowerSharedArrayLiteral(nm, declType, lit);
                    continue;
                }
            }
            llvm::Value* v = lowerExpr(initExpr);
            if (!slotTy) slotTy = v->getType();  // infer slot type from initializer
            llvm::Value* slot = entryAlloca(slotTy, nm);
            builder.CreateStore(coerceTo(v, slotTy, exprSigned(initExpr)), slot);
            values[nm] = slot;
            slotTypes[nm] = slotTy;
            signedness[nm] = typeIsSigned(declType);
        }
    }

    // Lower `Shared<T> name = shared T[size]` to one module-level addrspace(3)
    // global - shared memory is per block, not a per-thread alloca - and
    // register its decayed element pointer so tile[i] reuses the buffer path.
    void lowerSharedDecl(const std::string& nm, const CajetaTypePtr& declType,
                         const std::shared_ptr<NewExpression>& ne) {
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

        uint32_t swizStride = 0;
        if (declType && declType->toCanonical().compare(
                            0, std::string("cajeta.xpu.Swizzled").size(),
                            "cajeta.xpu.Swizzled") == 0) {
            auto cls = std::dynamic_pointer_cast<CajetaClass>(declType);
            if (!cls || cls->getTypeArguments().size() != 2)
                unsupported("Swizzled requires <T, S> (element type, row stride)");
            auto s = std::dynamic_pointer_cast<CajetaConstantType>(
                cls->getTypeArguments()[1]);
            if (!s) unsupported("Swizzled<T, S>: the row stride S must be an "
                                "integer constant");
            swizStride = (uint32_t) s->getValue();
            if (swizStride == 0 || (swizStride & (swizStride - 1)) != 0)
                unsupported("Swizzled<T, S>: the row stride S must be a power of "
                            "two (got " + std::to_string(swizStride) + ")");
        }

        // BlockPadded<T, Block, Pad>: insert `Pad` elements after every `Block`
        // logical elements (physical = a + (a/Block)*Pad).
        uint32_t blkPeriod = 0, blkPad = 0;
        if (declType && declType->toCanonical().compare(
                            0, std::string("cajeta.xpu.BlockPadded").size(),
                            "cajeta.xpu.BlockPadded") == 0) {
            auto cls = std::dynamic_pointer_cast<CajetaClass>(declType);
            if (!cls || cls->getTypeArguments().size() != 3)
                unsupported("BlockPadded requires <T, Block, Pad> (element type, "
                            "block size, pad Ã¢ÂÂ both in elements)");
            auto bk = std::dynamic_pointer_cast<CajetaConstantType>(
                cls->getTypeArguments()[1]);
            auto pd = std::dynamic_pointer_cast<CajetaConstantType>(
                cls->getTypeArguments()[2]);
            if (!bk || !pd)
                unsupported("BlockPadded<T, Block, Pad>: Block and Pad must be "
                            "integer constants");
            blkPeriod = (uint32_t) bk->getValue();
            blkPad = (uint32_t) pd->getValue();
            if (blkPeriod == 0)
                unsupported("BlockPadded<T, Block, Pad>: Block must be > 0");
        }

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
                uint64_t elemBytes = mod.getDataLayout().getTypeAllocSize(elemTy);
                if (elemBytes != 4)
                    unsupported("dynamic Shared<T> currently requires a 4-byte "
                                "element (the runtime's shared-length spec constant "
                                "assumes 4 bytes); got a " +
                                std::to_string(elemBytes) + "-byte element");
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
        // '_' not '.': the global's name becomes a target symbol and '.' is a
        // directive separator in PTX/AMDGCN asm.
        std::string gname = fn->getName().str() + "_" + nm;
        if (isDynamic && linkage == llvm::GlobalValue::InternalLinkage)
            gname = "cajeta_dynsh_" + gname;
        auto* gv = new llvm::GlobalVariable(
            mod, arrTy, /*isConstant=*/false, linkage, init,
            gname, /*InsertBefore=*/nullptr,
            llvm::GlobalValue::NotThreadLocal, kSharedAS);
        gv->setAlignment(llvm::MaybeAlign(16));

        if (isDynamic && linkage == llvm::GlobalValue::InternalLinkage) {
            arrayShared[nm] = {gv, arrTy};
            bufferElems[nm] = elemTy;
            bufferElemSigned[nm] = elemSigned;
            if (swizStride) swizzledBaseStride[gv] = swizStride;
            if (blkPeriod && blkPad) blockPadOfBase[gv] = {blkPeriod, blkPad};
            return;
        }
        llvm::Value* zero =
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), 0);
        llvm::Value* base = builder.CreateGEP(arrTy, gv, {zero, zero},
                                              nm + ".base");
        bufferBases[nm] = base;
        bufferElems[nm] = elemTy;
        bufferElemSigned[nm] = elemSigned;
        if (swizStride) swizzledBaseStride[base] = swizStride;
        if (blkPeriod && blkPad) blockPadOfBase[base] = {blkPeriod, blkPad};
    }

    // Lower `Shared<T> tile = shared [v0, v1, ...]` to a per-block addrspace(3)
    // tile of the literal's length and store its values in. Every thread runs
    // the store loop - redundant but idempotent, so no barrier is needed.
    void lowerSharedArrayLiteral(
            const std::string& nm, const CajetaTypePtr& declType,
            const std::shared_ptr<ArrayLiteralExpression>& lit) {
        llvm::Type* elemTy = nullptr;
        bool elemSigned = true;
        if (auto cls = std::dynamic_pointer_cast<CajetaClass>(declType)) {
            if (!cls->getTypeArguments().empty()) {
                elemTy = deviceScalarType(cls->getTypeArguments()[0], ctx);
                elemSigned = typeIsSigned(cls->getTypeArguments()[0]);
            }
        }
        if (!elemTy) unsupported("shared array literal '" + nm +
                                 "' needs a scalar element type (Shared<T>)");
        const auto& elems = lit->getElements();
        uint64_t n = elems.size();
        if (n == 0) unsupported("shared array literal '" + nm +
                                "' must be non-empty");

        llvm::ArrayType* arrTy = llvm::ArrayType::get(elemTy, n);
        std::string gname = fn->getName().str() + "_" + nm;
        auto* gv = new llvm::GlobalVariable(
            mod, arrTy, /*isConstant=*/false,
            llvm::GlobalValue::InternalLinkage,
            (llvm::Constant*) llvm::UndefValue::get(arrTy),
            gname, /*InsertBefore=*/nullptr,
            llvm::GlobalValue::NotThreadLocal, kSharedAS);
        gv->setAlignment(llvm::MaybeAlign(16));

        llvm::Value* zero =
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), 0);
        llvm::Value* base = builder.CreateGEP(arrTy, gv, {zero, zero},
                                              nm + ".base");
        bufferBases[nm] = base;
        bufferElems[nm] = elemTy;
        bufferElemSigned[nm] = elemSigned;

        // Every thread runs this loop, so the values MUST be compile-time
        // constants; a per-thread value would race on the same shared slots.
        for (uint64_t i = 0; i < n; ++i) {
            auto ex = std::dynamic_pointer_cast<Expression>(elems[i]);
            llvm::Value* v = coerceTo(lowerExpr(ex), elemTy, exprSigned(ex));
            if (!llvm::isa<llvm::Constant>(v)) {
                unsupported("shared array literal '" + nm + "' element " +
                    std::to_string(i) + " is not a compile-time constant; a "
                    "shared literal fills a per-block tile with constant values "
                    "Ã¢ÂÂ use `shared T[N]` and assign at runtime for computed "
                    "values");
            }
            llvm::Value* idx =
                llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), i);
            llvm::Value* slot = builder.CreateGEP(elemTy, base, idx);
            builder.CreateStore(v, slot);
        }
    }

    // Permute the element index `idx` through the backend's conflict-free
    // swizzle when `base` is a Swizzled<T,S> tile, else return `idx` unchanged.
    llvm::Value* maybeSwizzle(llvm::Value* base, llvm::Value* idx) {
        auto it = swizzledBaseStride.find(base);
        if (it != swizzledBaseStride.end())
            return target.swizzleAddr(builder, idx, it->second);
        auto bp = blockPadOfBase.find(base);
        if (bp != blockPadOfBase.end())
            return target.blockPadAddr(builder, idx, bp->second.first,
                                       bp->second.second);
        return idx;
    }

    // The swizzle row stride S if `e` is a bare identifier naming a Swizzled<T,S>
    // tile local, else 0 (not swizzled).
    uint32_t swizzleStrideOfArg(const ExpressionPtr& e) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(e)) {
            auto bb = bufferBases.find(id->getTextValue());
            if (bb != bufferBases.end()) {
                auto it = swizzledBaseStride.find(bb->second);
                if (it != swizzledBaseStride.end()) return it->second;
            }
        }
        return 0;
    }

    // The {block period, pad} of a BlockPadded tile named by `e`, else {0,0};
    // also returns the bare tile base in `*base`, which the additive pad needs.
    std::pair<uint32_t, uint32_t> blockPadOfArg(const ExpressionPtr& e,
                                                llvm::Value** base = nullptr) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(e)) {
            auto bb = bufferBases.find(id->getTextValue());
            if (bb != bufferBases.end()) {
                auto it = blockPadOfBase.find(bb->second);
                if (it != blockPadOfBase.end()) {
                    if (base) *base = bb->second;
                    return it->second;
                }
            }
        }
        return {0, 0};
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

    // for (init; cond; update) body Ã¢ÂÂ BB shape mirrors the host
    // (Statement.cpp): head(cond) / body / update / exit; continueÃ¢ÂÂupdate.
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
            : llvm::ConstantInt::getTrue(ctx);   // null cond Ã¢ÂÂ always-true
        builder.CreateCondBr(cond, body, exit);
        builder.SetInsertPoint(body);
        pushLoop(upd, exit);
        lowerStatement(fs->getBody());
        loopTargets.pop_back();
        if (!builder.GetInsertBlock()->hasTerminator()) builder.CreateBr(upd);
        builder.SetInsertPoint(upd);
        for (auto& u : fs->getUpdate()) lowerExprStatement(u);
        builder.CreateBr(head);
        builder.SetInsertPoint(exit);
    }

    // Grid-stride for-each: `for (idx, elem : buf.range(count))` becomes
    // `for (idx = globalId.x; idx < count; idx += gridSize.x)`. Device buffers
    // carry no length, so the iterable must spell the count explicitly.
    void lowerEnhancedFor(const std::shared_ptr<EnhancedForStatement>& efs) {
        auto mc = std::dynamic_pointer_cast<MethodCallExpression>(
            efs->getIterableExpr());

        if (mc && mc->getMethodCallName() == "stripe" &&
                !mc->getChildren().empty()) {
            std::string recvName;
            if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(
                    mc->getChildren()[0]))
                recvName = id->getTextValue();
            if (recvName == "Group") {
                if (mc->getParameters().size() != 1)
                    throw cajeta::Exception(
                        "XPU kernel lowering: Group.stripe(n) takes exactly one "
                        "argument", "XPU-N02");
                llvm::Type* si32 = llvm::Type::getInt32Ty(ctx);
                llvm::Type* idxTy = efs->getElementType()
                    ? deviceScalarType(efs->getElementType(), ctx) : si32;
                if (!idxTy || !idxTy->isIntegerTy()) idxTy = si32;
                bool idxSigned =
                    efs->getElementType() && typeIsSigned(efs->getElementType());
                const std::string& idxName = efs->getElementName();
                llvm::Value* count = coerceTo(
                    lowerExpr(mc->getParameters()[0].expression), idxTy);
                llvm::Value* start =
                    coerceTo(target.groupLaneId(builder, mod), idxTy);
                llvm::Value* step =
                    coerceTo(target.groupWidth(builder, mod), idxTy);
                llvm::Value* idxSlot = entryAlloca(idxTy, idxName);
                builder.CreateStore(start, idxSlot);
                values[idxName] = idxSlot;
                slotTypes[idxName] = idxTy;
                signedness[idxName] = idxSigned;

                auto* head = llvm::BasicBlock::Create(ctx, "stripe.head", fn);
                auto* body = llvm::BasicBlock::Create(ctx, "stripe.body", fn);
                auto* upd  = llvm::BasicBlock::Create(ctx, "stripe.update", fn);
                auto* exit = llvm::BasicBlock::Create(ctx, "stripe.exit", fn);
                builder.CreateBr(head);
                builder.SetInsertPoint(head);
                llvm::Value* i = builder.CreateLoad(idxTy, idxSlot, idxName);
                builder.CreateCondBr(
                    builder.CreateICmpULT(i, count, "stripe.cmp"), body, exit);
                builder.SetInsertPoint(body);
                pushLoop(upd, exit);
                lowerStatement(efs->getBody());
                loopTargets.pop_back();
                if (!builder.GetInsertBlock()->hasTerminator())
                    builder.CreateBr(upd);
                builder.SetInsertPoint(upd);
                llvm::Value* next = builder.CreateAdd(
                    builder.CreateLoad(idxTy, idxSlot, idxName), step,
                    "stripe.next");
                builder.CreateStore(next, idxSlot);
                builder.CreateBr(head);
                builder.SetInsertPoint(exit);
                return;
            }
        }

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

        llvm::Type* idxTy =
            efs->getIteratorType() ? deviceScalarType(efs->getIteratorType(), ctx)
                                   : i32;
        if (!idxTy || !idxTy->isIntegerTy()) idxTy = i32;
        bool idxSigned =
            efs->getIteratorType() && typeIsSigned(efs->getIteratorType());

        llvm::Value* count =
            coerceTo(lowerExpr(mc->getParameters()[0].expression), idxTy);
        llvm::Value* stride = coerceTo(target.gridSize(builder, mod, 0), idxTy);

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
        // Thread indices are non-negative, so an unsigned compare is correct.
        builder.CreateCondBr(builder.CreateICmpULT(i, count, "fe.cmp"),
                             body, exit);

        builder.SetInsertPoint(body);
        llvm::Value* i64idx =
            builder.CreateIntCast(i, i64, /*isSigned=*/idxSigned);
        llvm::Value* addr =
            target.bufferElementPtr(builder, mod, bv->second, elemTy, i64idx);
        builder.CreateStore(builder.CreateLoad(elemTy, addr, "fe.elem"),
                            elemSlot);
        pushLoop(upd, exit);
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

    // while (cond) body Ã¢ÂÂ head(cond) / body / exit; continueÃ¢ÂÂhead.
    void lowerWhile(const std::shared_ptr<WhileStatement>& ws) {
        auto* head = llvm::BasicBlock::Create(ctx, "while.head", fn);
        auto* body = llvm::BasicBlock::Create(ctx, "while.body", fn);
        auto* exit = llvm::BasicBlock::Create(ctx, "while.exit", fn);
        builder.CreateBr(head);
        builder.SetInsertPoint(head);
        builder.CreateCondBr(toI1(lowerExpr(ws->getCondition())), body, exit);
        builder.SetInsertPoint(body);
        pushLoop(head, exit);
        lowerStatement(ws->getBody());
        loopTargets.pop_back();
        if (!builder.GetInsertBlock()->hasTerminator()) builder.CreateBr(head);
        builder.SetInsertPoint(exit);
    }

    // do body while (cond) Ã¢ÂÂ body / tail(cond) / exit; continueÃ¢ÂÂtail.
    void lowerDo(const std::shared_ptr<DoStatement>& ds) {
        auto* body = llvm::BasicBlock::Create(ctx, "do.body", fn);
        auto* tail = llvm::BasicBlock::Create(ctx, "do.tail", fn);
        auto* exit = llvm::BasicBlock::Create(ctx, "do.exit", fn);
        builder.CreateBr(body);
        builder.SetInsertPoint(body);
        pushLoop(tail, exit);
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
        lowerExpr(expr);
    }

    // `lhs = rhs` (plain) and `lhs op= rhs` (compound: load-op-store). Works on
    // both scalar locals and buffer elements via lowerLValueAddr.
    void lowerAssign(const std::shared_ptr<BinaryOpExpression>& bin) {
        ExpressionPtr lhs = exprChild(bin, 0);
        ExpressionPtr rhs = exprChild(bin, 1);
        if (tryMatrixElementAssign(bin, lhs, rhs)) return;  // m[r][c] = Ã¢ÂÂ¦ (B1)
        if (tryVectorElementAssign(bin, lhs, rhs)) return;
        auto [addr, elemTy] = lowerLValueAddr(lhs);
        llvm::Value* rv = lowerExpr(rhs);
        BinaryOp op = bin->getBinaryOp();
        bool rvSigned = exprSigned(rhs);
        if (op != BINARY_OP_ASSIGN) {
            llvm::Value* cur = builder.CreateLoad(elemTy, addr, "cur");
            rv = applyBinOp(compoundBase(op), cur, rv, lvalueSigned(lhs),
                            elemTy->isFloatingPointTy());
            rvSigned = lvalueSigned(lhs);
        }
        builder.CreateStore(coerceTo(rv, elemTy, rvSigned), addr);
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
            auto sv = structValues.find(nm);
            if (sv != structValues.end()) return sv->second;
            unsupported("unbound identifier '" + nm + "'");
        }
        if (auto il = std::dynamic_pointer_cast<IntegerLiteralExpression>(expr)) {
            // Parse via APInt honoring the radix, never std::stoll: it reads the
            // wrong base, stops at `_`, and throws on overflow.
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
            unsigned width = 32;
            if (il->getResolvedType())
                if (llvm::Type* rt = deviceScalarType(il->getResolvedType(), ctx))
                    if (rt->isIntegerTy()) width = rt->getIntegerBitWidth();
            if (hasLSuffix || full.getActiveBits() > 32) width = 64;
            return llvm::ConstantInt::get(ctx, full.zextOrTrunc(width));
        }
        if (auto fl = std::dynamic_pointer_cast<FloatLiteralExpression>(expr)) {
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
        if (auto tl = std::dynamic_pointer_cast<TextLiteralExpression>(expr)) {
            if (tl->getLiteralType() == LITERAL_TYPE_BOOL) {
                return llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx),
                                              tl->getRawValue() == "true" ? 1 : 0);
            }
            if (tl->getLiteralType() == LITERAL_TYPE_STRING ||
                tl->getLiteralType() == LITERAL_TYPE_TEXT_BLOCK) {
                return builder.CreateGlobalString(decodeKernelString(tl->getRawValue()),
                                                  "kstr");
            }
            unsupported("text/string literal in kernel body");
        }
        if (auto ne = std::dynamic_pointer_cast<NewExpression>(expr)) {
            // Value types are tried before Vector so a user value type is not
            // mistaken for the builtin.
            if (llvm::Value* vt = lowerNewValueType(ne)) return vt;
            if (llvm::Value* mt = lowerNewMatrix(ne)) return mt;
            if (llvm::Value* qt = lowerNewQuaternion(ne)) return qt;
            return lowerNewVector(ne);
        }
        if (auto mc = std::dynamic_pointer_cast<MethodCallExpression>(expr)) {
            return lowerBuiltinCall(mc);
        }
        if (auto ai = std::dynamic_pointer_cast<ArrayIndexExpression>(expr)) {
            // Matrix indexing runs before the vector path: m[r] is a row, not a
            // flat lane.
            if (llvm::Value* me = matrixIndexRead(ai)) return me;
            if (llvm::Value* ve = vectorIndexRead(ai)) return ve;
            auto [addr, elemTy] = lowerLValueAddr(ai);
            return builder.CreateLoad(elemTy, addr, "elem");
        }
        if (auto dot = std::dynamic_pointer_cast<DotExpression>(expr)) {
            if (llvm::Value* fv = structFieldRead(expr)) return fv;
            if (llvm::Value* cv = vectorComponentRead(dot)) return cv;
            if (auto lhs = std::dynamic_pointer_cast<IdentifierExpression>(
                    exprChild(dot, 0))) {
                if (auto v = CajetaType::lookupEnumConstant(lhs->getTextValue(),
                                                            dot->getIdentifier()))
                    return llvm::ConstantInt::get(
                        llvm::Type::getInt32Ty(ctx), (uint64_t) *v,
                        /*isSigned=*/true);
            }
            unsupported("field access Ã¢ÂÂ only POD-struct kernel params and enum "
                        "constants support 'name.field'");
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
            CajetaTypePtr ct = cast->getResolvedType();
            if (!ct) ct = cast->getDestType();
            llvm::Type* dst = deviceScalarType(ct, ctx);
            if (!dst) unsupported("cast to non-scalar type");
            return castNumeric(v, dst, typeIsSigned(ct),
                               exprSigned(exprChild(cast, 0)));
        }
        if (auto call = std::dynamic_pointer_cast<CallExpression>(expr)) {
            auto callee = call->getCallee();
            if (auto ai = std::dynamic_pointer_cast<ArrayIndexExpression>(callee)) {
                if (auto baseId = std::dynamic_pointer_cast<IdentifierExpression>(
                        exprChild(ai, 0))) {
                    auto cit = callables.find(baseId->getTextValue());
                    if (cit != callables.end() && cit->second.isTable) {
                        llvm::Value* tag = lowerExpr(exprChild(ai, 1));
                        return emitCallableDispatch(cit->second, tag,
                                                    call->getArgs());
                    }
                }
            }
            unsupported("postfix call in kernel body (only a device dispatch "
                        "table `ops[i](...)` is callable here)");
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

    // Shared ++/-- on an l-value: load, ÃÂ±1, store. returnOld picks postfix
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

    // `new/stack Vec2(f0, f1, ...)` for a @ValueType constructible in this body
    // -> an SSA aggregate built by an insertvalue chain; nullptr when the name is
    // not a known value type. Arguments map positionally to declared fields.
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

    // Read m[r][c] on a matrix local: load the slot and extractelement at flat
    // lane r*C+c; nullptr when `ai` is not that nested shape. Must be tried
    // before vectorIndexRead - m[r] is a row, not a flat lane.
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

    // Method names lowerVectorMethod handles; used to decide whether a nameless
    // (chained) receiver is worth materializing into a slot.
    static bool isVectorMethodName(const std::string& n) {
        return n == "dot" || n == "dotAccum" || n == "length"
            || n == "normalize"
            || n == "widenLo" || n == "widenHi" || n == "narrow"
            || n == "toF32" || n == "toI32" || n == "toF16"
            || n == "asUnsigned" || n == "asSigned"
            || n == "asWords" || n == "asBytes" || n == "dotSum"
            || n == "lut4";
    }
    unsigned syntheticRecvSeq = 0;

    // The `<N x T>` slot type of a vector local `nm`, or nullptr if it is none.
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
        if (llvm::Value* w = narrowLaneWordExtract(vec, idx)) return w;
        return vecops::extractLane(builder, vec, idx);
    }

    // Read a runtime lane of a byte or half-word vector as a word extract plus a
    // shift: a divergent-index extractelement on `<16 x i8>` legalizes to a
    // fifteen-deep select chain on amdgpu. A constant lane is left alone.
    llvm::Value* narrowLaneWordExtract(llvm::Value* vec, llvm::Value* idx) {
        if (byteWordFormOff()) return nullptr;
        if (llvm::isa<llvm::Constant>(idx)) return nullptr;
        auto* vt = llvm::dyn_cast<llvm::FixedVectorType>(vec->getType());
        if (!vt) return nullptr;
        llvm::Type* et = vt->getElementType();
        unsigned bits = et->isIntegerTy() ? et->getIntegerBitWidth() : 0;
        if (bits != 8 && bits != 16) return nullptr;
        unsigned per = 32 / bits;                 // lanes per word
        unsigned n = vt->getNumElements();
        if (n % per != 0) return nullptr;
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        auto* wt = llvm::FixedVectorType::get(i32, n / per);
        llvm::Value* words = builder.CreateBitCast(vec, wt, "bv.words");
        llvm::Value* i = coerceTo(idx, i32, /*isSigned=*/false);
        unsigned laneShift = per == 4 ? 2 : 1;
        llvm::Value* wi = builder.CreateLShr(i, llvm::ConstantInt::get(i32, laneShift),
                                             "bv.word.idx");
        llvm::Value* word = builder.CreateExtractElement(words, wi, "bv.word");
        llvm::Value* sub = builder.CreateAnd(i, llvm::ConstantInt::get(i32, per - 1));
        llvm::Value* sh = builder.CreateShl(sub, llvm::ConstantInt::get(i32, bits == 8 ? 3 : 4),
                                            "bv.bit.shift");
        llvm::Value* picked = builder.CreateLShr(word, sh);
        return builder.CreateTrunc(picked, et, "vec.elt");
    }

    // `v.x = e` / `v[i] = e` and the compound forms: load the slot,
    // insertelement the lane, store back. Returns false when `lhs` is not a
    // vector-local component/index, so the caller takes the normal path.
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

    // Shared int8 dp4a dot-sum core: returns acc + sum_k self[k]*other[k] as
    // int32 over 4-byte chunks via integerDot4x8. `self`/`other` are <N x i8>
    // with N % 4 == 0. Used by both Vector.dotSum and Group.mac.
    llvm::Value* lowerInt8DotSumChunks(llvm::Value* self, llvm::Value* other,
                                       llvm::Value* acc, bool sgn) {
        auto* wvt = llvm::cast<llvm::FixedVectorType>(self->getType());
        unsigned n = wvt->getNumElements() / 4;
        auto* v4i8 = llvm::FixedVectorType::get(
            llvm::Type::getInt8Ty(builder.getContext()), 4);
        auto packedWordsN = [&](llvm::Value* v) -> llvm::Value* {
            for (int hop = 0; hop < 6; ++hop) {
                auto* ld = llvm::dyn_cast<llvm::LoadInst>(v);
                if (!ld) break;
                auto* al = llvm::dyn_cast<llvm::AllocaInst>(
                    ld->getPointerOperand());
                if (!al) break;
                llvm::StoreInst* only = nullptr;
                bool multi = false;
                for (llvm::User* u : al->users()) {
                    if (auto* st = llvm::dyn_cast<llvm::StoreInst>(u)) {
                        if (only) multi = true;
                        only = st;
                    }
                }
                if (!only || multi || only->getParent() != ld->getParent()
                        || !only->comesBefore(ld))
                    break;
                v = only->getValueOperand();
            }
            auto* bc = llvm::dyn_cast<llvm::BitCastInst>(v);
            if (!bc) return nullptr;
            auto* svt = llvm::dyn_cast<llvm::FixedVectorType>(bc->getSrcTy());
            if (svt && svt->getElementType()->isIntegerTy(32)
                    && svt->getNumElements() == n)
                return bc->getOperand(0);
            return nullptr;
        };
        llvm::Value* selfW = packedWordsN(self);
        llvm::Value* otherW = packedWordsN(other);
        llvm::Value* run = acc;
        for (unsigned lane = 0; lane < n; ++lane) {
            llvm::SmallVector<int, 4> m4 = {(int) (lane * 4),
                (int) (lane * 4 + 1), (int) (lane * 4 + 2),
                (int) (lane * 4 + 3)};
            llvm::Value* ws = selfW
                ? builder.CreateBitCast(
                      builder.CreateExtractElement(selfW, lane), v4i8,
                      "dotsum.w4")
                : builder.CreateShuffleVector(self, self, m4, "dotsum.w4");
            llvm::Value* as = otherW
                ? builder.CreateBitCast(
                      builder.CreateExtractElement(otherW, lane), v4i8,
                      "dotsum.a4")
                : builder.CreateShuffleVector(other, other, m4, "dotsum.a4");
            run = target.integerDot4x8(builder, mod, ws, as, run, sgn,
                                       /*cSigned=*/true);
        }
        return run;
    }

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
            // `dot` is SYMMETRIC: both operands take the receiver's signedness.
            // That is what makes it differ from dotAccum, deliberately.
            bool sgn = signedness.count(recv) ? signedness[recv] : true;
            return target.integerDot4x8(builder, mod, self, other, acc, sgn,
                                        sgn);
        }
        if (name == "asUnsigned" || name == "asSigned") {
            if (isFloat)
                unsupported("Vector.asUnsigned/asSigned require an integer "
                            "element type");
            if (!args.empty())
                unsupported("Vector.asUnsigned/asSigned take no arguments");
            return self;
        }
        if (name == "asWords" || name == "asBytes") {
            if (isFloat)
                unsupported("Vector.asWords/asBytes require an integer "
                            "element type");
            if (!args.empty())
                unsupported("Vector.asWords/asBytes take no arguments");
            unsigned lanes = vt->getNumElements();
            unsigned width = vt->getElementType()->getIntegerBitWidth();
            llvm::LLVMContext& c = builder.getContext();
            if (name == "asWords") {
                if (width != 8 || (lanes % 4) != 0)
                    unsupported("Vector.asWords needs an 8-bit vector whose "
                                "lane count is a multiple of 4");
                return builder.CreateBitCast(self,
                    llvm::FixedVectorType::get(llvm::Type::getInt32Ty(c),
                                               lanes / 4), "as.words");
            }
            if (width != 32)
                unsupported("Vector.asBytes needs a 32-bit vector");
            return builder.CreateBitCast(self,
                llvm::FixedVectorType::get(llvm::Type::getInt8Ty(c),
                                           lanes * 4), "as.bytes");
        }
        if (name == "dotSum") {
            if (isFloat)
                unsupported("Vector.dotSum is integer-only");
            if (args.size() != 2)
                unsupported("Vector.dotSum expects (other, acc)");
            auto* wvt = llvm::dyn_cast<llvm::FixedVectorType>(self->getType());
            if (wvt == nullptr
                    || wvt->getElementType()->getIntegerBitWidth() != 8
                    || (wvt->getNumElements() % 4) != 0)
                unsupported("Vector.dotSum needs an 8-bit vector whose lane "
                            "count is a multiple of 4");
            llvm::Value* other = lowerExpr(args[0].expression);
            llvm::Value* acc = lowerExpr(args[1].expression);
            auto* ovt = llvm::dyn_cast<llvm::FixedVectorType>(
                other->getType());
            if (ovt == nullptr
                    || ovt->getElementType()->getIntegerBitWidth() != 8
                    || ovt->getNumElements() != wvt->getNumElements())
                unsupported("Vector.dotSum's operands must have the same "
                            "8-bit lane count");
            if (!acc->getType()->isIntegerTy(32))
                unsupported("Vector.dotSum's accumulator must be int32");
            bool sgn = signedness.count(recv) ? signedness[recv] : true;
            return lowerInt8DotSumChunks(self, other, acc, sgn);
        }
        if (name == "lut4") {
            if (isFloat)
                unsupported("Vector.lut4 is integer-only");
            if (args.size() != 1)
                unsupported("Vector.lut4 expects (table)");
            auto* ivt = llvm::dyn_cast<llvm::FixedVectorType>(self->getType());
            if (ivt == nullptr
                    || ivt->getElementType()->getIntegerBitWidth() != 8)
                unsupported("Vector.lut4 needs an 8-bit index vector");
            llvm::Value* table = lowerExpr(args[0].expression);
            auto* tvt = llvm::dyn_cast<llvm::FixedVectorType>(table->getType());
            if (tvt == nullptr || !tvt->getElementType()->isIntegerTy(8)
                    || tvt->getNumElements() != 16)
                unsupported("Vector.lut4's table must be a 16-lane 8-bit "
                            "vector");
            return target.byteLut16(builder, mod, self, table);
        }
        if (name == "dotAccum") {
            if (isFloat)
                unsupported("Vector.dotAccum is integer-only");
            if (args.size() != 2)
                unsupported("Vector.dotAccum expects (other, acc)");
            auto* wvt = llvm::dyn_cast<llvm::FixedVectorType>(self->getType());
            if (wvt == nullptr
                    || wvt->getElementType()->getIntegerBitWidth() != 8
                    || (wvt->getNumElements() % 4) != 0)
                unsupported("Vector.dotAccum needs an 8-bit vector whose lane "
                            "count is a multiple of 4");
            llvm::Value* other = lowerExpr(args[0].expression);
            llvm::Value* accv = lowerExpr(args[1].expression);
            auto* avt = llvm::dyn_cast<llvm::FixedVectorType>(accv->getType());
            auto* ovt = llvm::dyn_cast<llvm::FixedVectorType>(
                other->getType());
            if (ovt == nullptr
                    || ovt->getElementType()->getIntegerBitWidth() != 8
                    || ovt->getNumElements() != wvt->getNumElements())
                unsupported("Vector.dotAccum's operands must have the same "
                            "8-bit lane count");
            if (avt == nullptr
                    || !avt->getElementType()->isIntegerTy(32)
                    || avt->getNumElements() * 4 != wvt->getNumElements())
                unsupported("Vector.dotAccum's accumulator must be "
                            "Vector<int32,N> for 4N lanes of int8");
            bool sgn = signedness.count(recv) ? signedness[recv] : true;
            if (llvm::Value* wide = target.integerDotWide(
                    builder, mod, self, other, accv, /*wUnsigned=*/!sgn))
                return wide;
            llvm::Type* i32d = llvm::Type::getInt32Ty(builder.getContext());
            unsigned n = avt->getNumElements();
            llvm::Value* out = accv;
            auto packedWords = [&](llvm::Value* v) -> llvm::Value* {
                // Strip load-of-single-store slots repeatedly to see through a
                // chained receiver; each hop needs store-dominates-load.
                for (int hop = 0; hop < 6; ++hop) {
                    auto* ld = llvm::dyn_cast<llvm::LoadInst>(v);
                    if (!ld) break;
                    auto* al = llvm::dyn_cast<llvm::AllocaInst>(
                        ld->getPointerOperand());
                    if (!al) break;
                    llvm::StoreInst* only = nullptr;
                    bool multi = false;
                    for (llvm::User* u : al->users()) {
                        if (auto* st = llvm::dyn_cast<llvm::StoreInst>(u)) {
                            if (only) multi = true;
                            only = st;
                        }
                    }
                    if (!only || multi || only->getParent() != ld->getParent()
                            || !only->comesBefore(ld))
                        break;
                    v = only->getValueOperand();
                }
                auto* bc = llvm::dyn_cast<llvm::BitCastInst>(v);
                if (!bc) return nullptr;
                auto* svt = llvm::dyn_cast<llvm::FixedVectorType>(
                    bc->getSrcTy());
                if (svt && svt->getElementType()->isIntegerTy(32)
                        && svt->getNumElements() == n)
                    return bc->getOperand(0);
                return nullptr;
            };
            llvm::Value* selfW = packedWords(self);
            llvm::Value* otherW = packedWords(other);
            auto* v4i8 = llvm::FixedVectorType::get(
                llvm::Type::getInt8Ty(builder.getContext()), 4);
            for (unsigned lane = 0; lane < n; ++lane) {
                llvm::SmallVector<int, 4> m4 = {(int) (lane * 4),
                    (int) (lane * 4 + 1), (int) (lane * 4 + 2),
                    (int) (lane * 4 + 3)};
                llvm::Value* ws = selfW
                    ? builder.CreateBitCast(
                          builder.CreateExtractElement(selfW, lane,
                                                       "dotacc.w32"),
                          v4i8, "dotacc.w4")
                    : builder.CreateShuffleVector(self, self, m4,
                                                  "dotacc.w4");
                llvm::Value* as = otherW
                    ? builder.CreateBitCast(
                          builder.CreateExtractElement(otherW, lane,
                                                       "dotacc.a32"),
                          v4i8, "dotacc.a4")
                    : builder.CreateShuffleVector(other, other, m4,
                                                  "dotacc.a4");
                llvm::Value* a0 = builder.CreateExtractElement(out, lane,
                                                               "dotacc.acc");
                // dotAccum is ASYMMETRIC: the host contract reads only the
                // receiver's signedness and always sign-extends the activations.
                llvm::Value* r = target.integerDot4x8(builder, mod, ws, as,
                    builder.CreateIntCast(a0, i32d, true), sgn,
                    /*cSigned=*/true);
                out = builder.CreateInsertElement(out, r, lane, "dotacc.ins");
            }
            return out;
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
            // Signedness comes from the RECEIVER's declared element type - sext
            // for a signed element, zext for an unsigned one.
        if (name == "widenLo" || name == "widenHi") {
            if (isFloat)
                unsupported("Vector.widenLo/widenHi require an integer "
                            "element type");
            if (!args.empty())
                unsupported("Vector.widenLo/widenHi take no arguments");
            unsigned w = elemTy->getIntegerBitWidth();
            unsigned n = vt->getNumElements();
            if (w >= 64 || n < 2)
                unsupported("Vector.widen*: element width must be < 64 and "
                            "lane count >= 2");
            bool sgn = signedness.count(recv) ? signedness[recv] : true;
            return vecops::widenHalf(builder, self, name == "widenLo", sgn);
        }
        if (name == "narrow") {
            if (isFloat)
                unsupported("Vector.narrow requires an integer element type");
            if (args.size() != 1)
                unsupported("Vector.narrow expects (other) Ã¢ÂÂ the high half's "
                            "source");
            if (elemTy->getIntegerBitWidth() <= 8)
                unsupported("Vector.narrow: element width must be > 8");
            llvm::Value* other = lowerExpr(args[0].expression);
            if (other->getType() != self->getType())
                unsupported("Vector.narrow: other must have the receiver's "
                            "Vector type");
            return vecops::narrowPair(builder, self, other);
        }
        if (name == "toF32") {
            if (!args.empty())
                unsupported("Vector.toF32 takes no arguments");
            if (isFloat) {
                if (elemTy->getPrimitiveSizeInBits() >= 32)
                    unsupported("Vector.toF32 needs an integer or "
                                "narrower-float element type "
                                "(float16 / bfloat16)");
                return vecops::convertFpLanes(builder, self,
                    llvm::Type::getFloatTy(builder.getContext()));
            }
            bool sgn = signedness.count(recv) ? signedness[recv] : true;
            return vecops::convertToF32(builder, self, sgn);
        }
        if (name == "toF16") {
            if (!isFloat || !args.empty())
                unsupported("Vector.toF16 takes no arguments and a "
                            "float-element receiver");
            return vecops::convertFpLanes(builder, self,
                llvm::Type::getHalfTy(builder.getContext()));
        }
        if (name == "toI32") {
            if (!isFloat || !args.empty())
                unsupported("Vector.toI32 takes no arguments and a "
                            "float-element receiver");
            return vecops::convertToI32(builder, self);
        }
        unsupported("unknown Vector method '" + name + "'");
    }

    // `m.transpose()/identity()/row(r)/col(c)/hadamard(b)` on a matrix local,
    // producing the flat `<R*C x T>` / `<C x T>` / `<R x T>` value. Must be
    // dispatched before lowerVectorMethod: a matrix slot is a vector type too.
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
    // unknown field Ã¢ÂÂ the last surfaces as `unsupported` only at access time).
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
    // param's SSA aggregate - no pointer, so it is valid in logical addressing.
    // Returns nullptr when `e` is not a field access; throws on an unknown field.
    llvm::Value* structFieldRead(const ExpressionPtr& e) {
        auto dot = std::dynamic_pointer_cast<DotExpression>(e);
        if (!dot || dot->getChildren().empty()) return nullptr;
        auto baseExpr = std::dynamic_pointer_cast<Expression>(dot->getChildren()[0]);
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
        if (structFieldOf(e))
            unsupported("POD-struct kernel params are read-only "
                        "(no 'name.field = ...')");
        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(e)) {
            const std::string& nm = id->getTextValue();
            auto it = values.find(nm);
            if (it == values.end())
                unsupported("assignment to unbound local '" + nm + "'");
            return {it->second, slotTypes[nm]};
        }
        auto ai = std::dynamic_pointer_cast<ArrayIndexExpression>(e);
        if (!ai) unsupported("l-value that isn't a buffer index or local");
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
        if (auto as = arrayShared.find(baseId->getTextValue());
            as != arrayShared.end()) {
            llvm::Value* idx = lowerExpr(exprChild(ai, 1));
            llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
            if (idx->getType() != i64)
                idx = builder.CreateIntCast(idx, i64,
                                            exprSigned(exprChild(ai, 1)));
            idx = maybeSwizzle(as->second.first, idx);
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
        if (idx->getType() != llvm::Type::getInt64Ty(ctx)) {
            idx = builder.CreateIntCast(idx, llvm::Type::getInt64Ty(ctx),
                                        exprSigned(idxExpr));
        }
        idx = maybeSwizzle(bv->second, idx);
        llvm::Value* addr =
            target.bufferElementPtr(builder, mod, bv->second, be->second, idx);
        return {addr, be->second};
    }

    // Device builtins (Thread / Workgroup coordinates, Barrier). The name to
    // coordinate mapping is shared; only the leaf intrinsic is per-backend.
    llvm::Value* lowerBuiltinCall(const std::shared_ptr<MethodCallExpression>& mc) {
        std::string recv;
        if (!mc->getChildren().empty()) {
            if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(
                    mc->getChildren()[0])) {
                recv = id->getTextValue();
            }
        }
        const std::string& name = mc->getMethodCallName();

        if ((name == "vload" || name == "vstore")
                && bufferBases.count(recv) && bufferElems.count(recv)) {
            llvm::Type* elemTy = bufferElems[recv];
            llvm::Value* base = bufferBases[recv];
            const auto& params = mc->getParameters();
            auto toI64 = [&](const ExpressionPtr& e) {
                llvm::Value* v = lowerExpr(e);
                if (v->getType() != llvm::Type::getInt64Ty(ctx))
                    v = builder.CreateIntCast(
                        v, llvm::Type::getInt64Ty(ctx), exprSigned(e));
                return v;
            };
            if (name == "vload") {
                const auto& targs = mc->getExplicitMethodTypeArgs();
                if (targs.size() != 1)
                    unsupported("vload<N> requires one const lane-count arg");
                auto cN = std::dynamic_pointer_cast<CajetaConstantType>(targs[0]);
                if (!cN) unsupported("vload<N>: N must be an integer constant");
                if (params.size() != 1) unsupported("vload<N> expects (index)");
                llvm::Value* idx = maybeSwizzle(base, toI64(params[0].expression));
                return target.vectorLoad(builder, mod, base, elemTy,
                                         (unsigned) cN->getValue(), idx);
            }
            if (params.size() != 2)
                unsupported("vstore expects (index, value)");
            llvm::Value* idx = maybeSwizzle(base, toI64(params[0].expression));
            llvm::Value* val = lowerExpr(params[1].expression);
            if (!val->getType()->isVectorTy())
                unsupported("vstore value must be a Vector<T,N>");
            unsigned lanes = llvm::cast<llvm::FixedVectorType>(
                val->getType())->getNumElements();
            target.vectorStore(builder, mod, base, elemTy, lanes, idx, val);
            return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
        }

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

        if (recv.empty()) {
            auto cit = callables.find(name);
            if (cit != callables.end() && !cit->second.isTable)
                return emitCallableDispatch(cit->second, cit->second.tagVal,
                                            mc->getParameters());
        }

        if (recv == "KernelThread") {
            if (name == "x") return target.threadId(builder, mod, 0);
            if (name == "y") return target.threadId(builder, mod, 1);
            if (name == "z") return target.threadId(builder, mod, 2);
            if (name == "globalIdX") return target.globalId(builder, mod, 0);
            if (name == "globalIdY") return target.globalId(builder, mod, 1);
            if (name == "globalIdZ") return target.globalId(builder, mod, 2);
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
            if (name == "workgroupMemory" || name == "deviceMemory") {
                const auto& fargs = mc->getParameters();
                if (fargs.size() > 1)
                    unsupported("Barrier." + name + " expects ([order])");
                LoweringTarget::MemoryOrder order =
                    LoweringTarget::MemoryOrder::Default;
                if (fargs.size() == 1) {
                    auto* ci = llvm::dyn_cast<llvm::ConstantInt>(
                        lowerExpr(fargs[0].expression));
                    if (!ci)
                        unsupported("memory order must be a compile-time "
                                    "MemoryOrder constant");
                    uint64_t k = ci->getZExtValue();
                    if (k > 4) unsupported("invalid MemoryOrder value");
                    order = static_cast<LoweringTarget::MemoryOrder>((int) k);
                }
                target.memoryFence(builder, mod,
                                   name == "workgroupMemory"
                                       ? LoweringTarget::FenceScope::Workgroup
                                       : LoweringTarget::FenceScope::Device,
                                   order);
                return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
            }
        } else if (recv == "Debug") {
            if (name == "printf") {
                const auto& args = mc->getParameters();
                if (args.empty())
                    unsupported("Debug.printf needs a format string");
                llvm::Value* fmt = lowerExpr(args[0].expression);
                std::vector<llvm::Value*> pargs;
                pargs.reserve(args.size() - 1);
                for (size_t i = 1; i < args.size(); ++i)
                    pargs.push_back(lowerExpr(args[i].expression));
                target.devicePrintf(builder, mod, fmt, pargs);
                return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
            }
        } else if (recv == "Spec") {
            if (name == "geti") {
                const auto& args = mc->getParameters();
                if (args.size() != 2)
                    unsupported("Spec.geti expects (slot, default) Ã¢ÂÂ two "
                                "compile-time i32 constants");
                auto* slotC = llvm::dyn_cast<llvm::ConstantInt>(
                    lowerExpr(args[0].expression));
                auto* defC = llvm::dyn_cast<llvm::ConstantInt>(
                    lowerExpr(args[1].expression));
                if (!slotC || !defC)
                    unsupported("Spec.geti(slot, default) needs compile-time "
                                "i32 constants");
                uint64_t slot = slotC->getZExtValue();
                if (slot >= LoweringTarget::kMaxUserSpecConstants)
                    unsupported("Spec.geti slot must be < " +
                                std::to_string(
                                    LoweringTarget::kMaxUserSpecConstants));
                return target.specConstantI32(builder, mod, (unsigned) slot,
                                              (int32_t) defC->getSExtValue());
            }
            if (name == "getf") {
                const auto& args = mc->getParameters();
                if (args.size() != 2)
                    unsupported("Spec.getf expects (slot, default) Ã¢ÂÂ a "
                                "compile-time i32 slot and f32 default");
                auto* slotC = llvm::dyn_cast<llvm::ConstantInt>(
                    lowerExpr(args[0].expression));
                auto* defC = llvm::dyn_cast<llvm::ConstantFP>(
                    lowerExpr(args[1].expression));
                if (!slotC || !defC)
                    unsupported("Spec.getf(slot, default) needs a compile-time "
                                "i32 slot and an f32 default constant");
                uint64_t slot = slotC->getZExtValue();
                if (slot >= LoweringTarget::kMaxUserSpecConstants)
                    unsupported("Spec.getf slot must be < " +
                                std::to_string(
                                    LoweringTarget::kMaxUserSpecConstants));
                llvm::APFloat apf = defC->getValueAPF();
                bool lost = false;
                apf.convert(llvm::APFloat::IEEEsingle(),
                            llvm::APFloat::rmNearestTiesToEven, &lost);
                return target.specConstantF32(builder, mod, (unsigned) slot,
                                              apf.convertToFloat());
            }
        } else if (recv == "Wave") {
            const auto& args = mc->getParameters();
            if (name == "width") return target.waveWidth(builder, mod);
            if (name == "laneId") return target.waveLaneId(builder, mod);
            if (name == "isFirstLane") {
                llvm::Value* lane = target.waveLaneId(builder, mod);
                return builder.CreateICmpEQ(
                    lane, llvm::ConstantInt::get(lane->getType(), 0),
                    "wave.isfirst");
            }
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
            if (name == "reduceSumF32" || name == "reduceMaxF32") {
                if (args.size() != 1) unsupported("Wave.reduceF32 arity");
                usedSubgroupOp_ = true;
                auto fop = name == "reduceSumF32"
                    ? LoweringTarget::WaveReduceFOp::Sum
                    : LoweringTarget::WaveReduceFOp::Max;
                return target.waveReduceF32(builder, mod, fop,
                                            lowerExpr(args[0].expression));
            }
            if (name == "reduceSumF32Segmented"
                    || name == "reduceMaxF32Segmented") {
                if (args.size() != 2) unsupported("Wave.reduceF32Segmented arity");
                usedSubgroupOp_ = true;
                auto fop = name == "reduceSumF32Segmented"
                    ? LoweringTarget::WaveReduceFOp::Sum
                    : LoweringTarget::WaveReduceFOp::Max;
                return target.waveReduceF32Segmented(
                    builder, mod, fop,
                    lowerExpr(args[0].expression),
                    lowerExpr(args[1].expression));
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
        } else if (recv == "TargetDescriptor") {
            if (name == "waveWidth") return target.groupWidth(builder, mod);
            unsupported("TargetDescriptor." + name + " is not a device op "
                        "(only waveWidth() folds on the hot path; the machine "
                        "estimates are host-side)");
        } else if (recv == "Group") {
            const auto& args = mc->getParameters();
            if (name == "width") return target.groupWidth(builder, mod);
            if (name == "laneId") return target.groupLaneId(builder, mod);
            if (name == "rowId") return target.workgroupId(builder, mod, 0);
            auto groupFop = [&](const ExpressionPtr& e)
                    -> LoweringTarget::WaveReduceFOp {
                auto* c = llvm::dyn_cast<llvm::ConstantInt>(lowerExpr(e));
                if (!c) unsupported("Group.reduce op must be a compile-time "
                                    "GroupOp constant (GroupOp.Add / .Max)");
                uint64_t ord = c->getZExtValue();
                if (ord == 0) return LoweringTarget::WaveReduceFOp::Sum; // Add
                if (ord == 1) return LoweringTarget::WaveReduceFOp::Max;
                unsupported("Group.reduce: unknown GroupOp ordinal "
                            + std::to_string(ord) + " (Add=0, Max=1)");
                return LoweringTarget::WaveReduceFOp::Sum; // unreachable
            };
            if (name == "reduce") {
                if (args.size() != 2)
                    unsupported("Group.reduce(op, value) arity");
                usedSubgroupOp_ = true;
                auto fop = groupFop(args[0].expression);
                return target.groupReduceF32(builder, mod, fop,
                                             lowerExpr(args[1].expression));
            }
            if (name == "reduceSegmented") {
                if (args.size() != 3)
                    unsupported("Group.reduceSegmented(seg, op, value) arity");
                usedSubgroupOp_ = true;
                llvm::Value* seg = lowerExpr(args[0].expression);
                auto fop = groupFop(args[1].expression);
                return target.groupReduceF32Segmented(
                    builder, mod, fop, lowerExpr(args[2].expression), seg);
            }
            if (name == "mac") {
                if (args.size() != 3)
                    unsupported("Group.mac(acc, a, b) arity");
                if (auto accId = std::dynamic_pointer_cast<IdentifierExpression>(
                        args[0].expression)) {
                    if (coopMatrixSlots.count(accId->getTextValue())) {
                        auto sub = std::make_shared<MethodCallExpression>(
                            "mma",
                            std::vector<cajeta::MethodCallParameter>{
                                args[1], args[2]});
                        return lowerCoopMatrixMethod(accId->getTextValue(),
                                                     "mma", sub);
                    }
                }
                llvm::Value* accV = lowerExpr(args[0].expression);
                llvm::Value* aV = lowerExpr(args[1].expression);
                llvm::Value* bV = lowerExpr(args[2].expression);
                auto* avt = llvm::dyn_cast<llvm::FixedVectorType>(aV->getType());
                auto* bvt = llvm::dyn_cast<llvm::FixedVectorType>(bV->getType());
                if (avt && bvt
                        && avt->getElementType()->isIntegerTy(8)
                        && bvt->getElementType()->isIntegerTy(8)
                        && avt->getNumElements() == bvt->getNumElements()
                        && (avt->getNumElements() % 4) == 0
                        && accV->getType()->isIntegerTy(32)) {
                    bool sgn = true;
                    if (auto id =
                            std::dynamic_pointer_cast<IdentifierExpression>(
                                args[1].expression))
                        if (signedness.count(id->getTextValue()))
                            sgn = signedness[id->getTextValue()];
                    return lowerInt8DotSumChunks(aV, bV, accV, sgn);
                }
                unsupported("Group.mac: the int8 dp4a tier needs (int32 acc, "
                            "Vector<int8,N> a, Vector<int8,N> b), N % 4 == 0; "
                            "the f16/bf16 WMMA tile tier is not yet wired");
            }
            if (name == "stripe")
                unsupported("Group.stripe(n) is only a for-each iterable: "
                            "`for (int32 i : Group.stripe(n)) { ... }`");
            unsupported("Group." + name + " is not a device op");
        } else if (recv == "Quad") {
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
        } else if (recv == "Cajeta") {
            const auto& cargs = mc->getParameters();
            if ((name == "f32ToBits" || name == "bitsToF32" ||
                 name == "f64ToBits" || name == "bitsToF64") &&
                cargs.size() == 1) {
                llvm::LLVMContext& bc = builder.getContext();
                llvm::Value* v = lowerExpr(cargs[0].expression);
                // COERCE the operand to the width the bitcast needs: bitsToF32 is
                // routinely handed an int64, and bitcast i64 -> float is illegal.
                llvm::Type* to =
                    name == "f32ToBits"
                        ? (llvm::Type*) llvm::Type::getInt32Ty(bc)
                  : name == "bitsToF32"
                        ? (llvm::Type*) llvm::Type::getFloatTy(bc)
                  : name == "f64ToBits"
                        ? (llvm::Type*) llvm::Type::getInt64Ty(bc)
                        : (llvm::Type*) llvm::Type::getDoubleTy(bc);
                llvm::Type* from =
                    name == "f32ToBits"   ? (llvm::Type*) llvm::Type::getFloatTy(bc)
                  : name == "bitsToF32"   ? (llvm::Type*) llvm::Type::getInt32Ty(bc)
                  : name == "f64ToBits"   ? (llvm::Type*) llvm::Type::getDoubleTy(bc)
                                          : (llvm::Type*) llvm::Type::getInt64Ty(bc);
                v = coerceTo(v, from, /*isSigned=*/true);
                return builder.CreateBitCast(v, to, "cajeta.bits");
            }
            unsupported("Cajeta." + name + " in a kernel (supported: "
                        "f32ToBits, bitsToF32, f64ToBits, bitsToF64)");
        } else if (recv == "Bits") {
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
                // Deliberately NOT llvm.fshl/fshr: the SPIR-V backend lowers
                // those through a generated helper function with external
                // linkage, which pulls in OpCapability Linkage and Vulkan rejects.
                auto* w = builder.getInt32(32);
                auto* mask = builder.getInt32(31);
                llvm::Value* s = builder.CreateAnd(amt, mask, "bits.rot.s");
                // (32 - s) & 31 keeps the complementary shift in [0,31], so s == 0
                // is a 0-shift rather than an undef shift-by-32.
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
            return lowerMathCall(name, mc);
        } else if (recv == "CoopStage") {
            return lowerCoopStage(name, mc);
        } else if (recv == "AsyncCopy") {
            return lowerAsyncCopy(name, mc);
        } else if (recv == "Schedule") {
            return lowerSchedule(name, mc);
        }
        if (!recv.empty() && bufferBases.count(recv) &&
            (name == "atomicAdd" || name == "atomicSub" || name == "atomicMin" ||
             name == "atomicMax" || name == "atomicAnd" || name == "atomicOr" ||
             name == "atomicXor" || name == "atomicExchange" ||
             name == "atomicCompareExchange")) {
            const auto& args = mc->getParameters();
            const bool isCas = (name == "atomicCompareExchange");
            const size_t baseArgs = isCas ? 3 : 2;
            const bool hasOrder = (args.size() == baseArgs + 1);
            if (args.size() != baseArgs && !hasOrder)
                unsupported("Buffer." + name +
                            (isCas ? " expects (index, expected, desired[, order])"
                                   : " expects (index, value[, order])"));
            auto parseOrder = [&](const auto& e) -> LoweringTarget::MemoryOrder {
                auto* ci = llvm::dyn_cast<llvm::ConstantInt>(lowerExpr(e));
                if (!ci)
                    unsupported("memory order must be a compile-time "
                                "MemoryOrder constant");
                uint64_t k = ci->getZExtValue();
                if (k > 4) unsupported("invalid MemoryOrder value");
                return static_cast<LoweringTarget::MemoryOrder>((int) k);
            };
            LoweringTarget::MemoryOrder order =
                hasOrder ? parseOrder(args[baseArgs].expression)
                         : LoweringTarget::MemoryOrder::Default;
            llvm::Type* elemTy = bufferElems[recv];
            const bool elemSigned =
                bufferElemSigned.count(recv) ? bufferElemSigned[recv] : true;
            const bool elemIsFloat = elemTy->isFloatTy();
            const bool elemIsInt =
                elemTy->isIntegerTy(32) || elemTy->isIntegerTy(64);
            llvm::Value* idx = lowerExpr(args[0].expression);
            llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
            if (idx->getType() != i64)
                idx = builder.CreateIntCast(idx, i64,
                                            exprSigned(args[0].expression));
            llvm::Value* ptr = target.bufferElementPtr(builder, mod,
                                                       bufferBases[recv],
                                                       elemTy, idx);
            if (isCas) {
                if (!elemIsInt)
                    unsupported("Buffer.atomicCompareExchange requires an "
                                "int32/uint32/int64/uint64 buffer");
                llvm::Value* expected =
                    coerceTo(lowerExpr(args[1].expression), elemTy);
                llvm::Value* desired =
                    coerceTo(lowerExpr(args[2].expression), elemTy);
                return target.atomicCompareExchange(builder, mod, ptr, expected,
                                                    desired, order);
            }
            llvm::Value* val = coerceTo(lowerExpr(args[1].expression), elemTy);
            if (elemIsFloat) {
                if (name == "atomicAdd" || name == "atomicMin" ||
                    name == "atomicMax") {
                    LoweringTarget::AtomicFloatOp op =
                        name == "atomicAdd" ? LoweringTarget::AtomicFloatOp::Add
                      : name == "atomicMin" ? LoweringTarget::AtomicFloatOp::Min
                                            : LoweringTarget::AtomicFloatOp::Max;
                    return target.atomicFloatRMW(builder, mod, op, ptr, val,
                                                 order);
                }
                unsupported("Buffer." + name + " is not supported on float32 "
                            "buffers (float atomics: atomicAdd/atomicMin/"
                            "atomicMax only)");
            }
            if (elemIsInt) {
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
                                           elemSigned, order);
            }
            unsupported("Buffer." + name + " requires a float32, int32/uint32, "
                        "or int64/uint64 buffer");
        }
        // Checked BEFORE the vector branch: a matrix local's slot is itself a
        // `<R*C x T>` vector type.
        if (!recv.empty() && matrixShapes.count(recv)) {
            return lowerMatrixMethod(recv, name, mc);
        }
        // Checked BEFORE the vector branch: a quaternion local's slot is `<4 x T>`.
        if (!recv.empty() && quaternionLocals.count(recv)) {
            return lowerQuaternionMethod(recv, name, mc);
        }
        if (!recv.empty() && vectorSlotType(recv)) {
            return lowerVectorMethod(recv, name, mc);
        }
        if (recv.empty() && isVectorMethodName(name)
                && !mc->getChildren().empty()) {
            ExpressionPtr recvExpr = std::dynamic_pointer_cast<Expression>(
                mc->getChildren()[0]);
            if (!recvExpr) return nullptr;
            llvm::Value* rv = lowerExpr(recvExpr);
            if (rv && rv->getType()->isVectorTy()) {
                auto* rvt = llvm::cast<llvm::FixedVectorType>(rv->getType());
                const std::string tmp =
                    ".vrecv." + std::to_string(syntheticRecvSeq++);
                // entryAlloca, not a point-of-use alloca: mem2reg promotes only
                // the entry block, so a chained-receiver spill inside a loop
                // would stay a memory round-trip and re-alloca per iteration.
                llvm::Value* slot = entryAlloca(rvt, tmp);
                builder.CreateStore(rv, slot);
                values[tmp]      = slot;
                slotTypes[tmp]   = rvt;
                signedness[tmp]  = exprSigned(recvExpr);
                return lowerVectorMethod(tmp, name, mc);
            }
        }
        if (auto rq = rayQuerySlots.find(recv); rq != rayQuerySlots.end()) {
            return lowerRayQueryMethod(rq->second, name, mc);
        }
        if (auto cm = coopMatrixSlots.find(recv); cm != coopMatrixSlots.end()) {
            return lowerCoopMatrixMethod(recv, name, mc);
        }
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
                if (auto tt = textureTexelTypes.find(recv);
                        tt != textureTexelTypes.end() &&
                        tt->second && tt->second->isIntegerTy()) {
                    throw cajeta::Exception(
                        "Texture<int>.sample is not supported Ã¢ÂÂ integer textures "
                        "cannot be filtered by the hardware sampler; use fetch(...) "
                        "for an exact integer-texel read", "XPU-N01");
                }
                llvm::Value* samp = resolveSamplerArg(args[0].expression);
                llvm::Value* u = toFloat(lowerExpr(args[1].expression));
                if (dim == 1)
                    return target.sampleTexture1D(builder, mod, th->second, samp, u);
                llvm::Value* v = toFloat(lowerExpr(args[2].expression));
                if (dim == 4) {
                    llvm::Value* layer = toI32(lowerExpr(args[3].expression));
                    return target.sampleTexture2DArray(builder, mod, th->second,
                                                       samp, u, v, layer);
                }
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
                llvm::Value* lod = isLod
                    ? toFloat(lowerExpr(args[3].expression))
                    : llvm::ConstantFP::get(llvm::Type::getFloatTy(mod.getContext()), 0.0);
                return target.sampleTexture(builder, mod, th->second, samp, u, v,
                                            lod);
            }
        }
        if (name == "fetch" || name == "fetchLod") {
            auto th = textureHandles.find(recv);
            if (th != textureHandles.end()) {
                bool isLod = (name == "fetchLod");
                int dim = 2;
                if (auto d = textureDims.find(recv); d != textureDims.end())
                    dim = d->second;
                if (isLod && dim != 2)
                    unsupported("fetchLod is supported on Texture2D only");
                if (dim == 5)
                    unsupported("TextureCube has no fetch Ã¢ÂÂ sample(s, x, y, z) "
                                "reads a cube by direction vector");
                const auto& args = mc->getParameters();
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
                llvm::Type* texelTy = llvm::Type::getFloatTy(mod.getContext());
                if (auto tt = textureTexelTypes.find(recv);
                        tt != textureTexelTypes.end() && tt->second)
                    texelTy = tt->second;
                if (dim == 1)
                    return target.fetchTexture1D(builder, mod, th->second, x, texelTy);
                llvm::Value* y = toI32(lowerExpr(args[1].expression));
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
                llvm::Value* lod = isLod
                    ? toI32(lowerExpr(args[2].expression))
                    : llvm::ConstantInt::get(llvm::Type::getInt32Ty(mod.getContext()), 0);
                return target.fetchTexture(builder, mod, th->second, x, y,
                                           texelTy, lod);
            }
        }
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
                return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
            }
        }
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

    // Resolve the sampler argument of `tex.sample(sampler, ...)` to its
    // materialized descriptor; it must be a bare identifier naming a Sampler
    // param, since a sampler is a bound resource, not an expressible value.
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
    // dispatch into cajeta.xpu.SoftwareRayQuery / SwRayCursor.
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
            auto c = coreClass("cajeta.xpu.SwRayCursor");
            swCursorInfoCache = c ? deviceStructInfo(c, ctx) : DeviceStructInfo{};
            swCursorCached = true;
        }
        return swCursorInfoCache;
    }

    // Software-tier RayQuery op lowering: the RayQuery alloca holds a
    // SwRayCursor, and each op is a call into the portable SoftwareRayQuery walk
    // with the cursor read and written back, plus field reads off the cursor.
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
            rayQueryBvh[rqPtr] = resolveAccelArg(args[0].expression);
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
                coreDeviceMethod("cajeta.xpu.SoftwareRayQuery", "step");
            if (!m) unsupported("cajeta.xpu.SoftwareRayQuery.step unavailable");
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

    // RayQuery op dispatch: `rqPtr` is the RayQuery alloca. Lowered to the
    // backend ray-query seam, or to the portable SoftwareRayQuery walk.
    llvm::Value* lowerRayQueryMethod(
            llvm::Value* rqPtr, const std::string& name,
            const std::shared_ptr<MethodCallExpression>& mc) {
        if (target.softwareRayQuery())
            return lowerSoftwareRayQueryMethod(rqPtr, name, mc);
        const auto& args = mc->getParameters();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        if (name == "initialize") {
            // (AccelerationStructure, rayFlags, cullMask, ox, oy, oz, tMin,
            //  dx, dy, dz, tMax) - origin/direction arrive as component scalars
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
            return llvm::ConstantInt::get(i32, 0);
        }
        if (name == "proceed") {
            if (!args.empty()) unsupported("RayQuery.proceed takes no arguments");
            return target.rayQueryProceed(builder, mod, rqPtr);
        }
        if (name == "committedType" || name == "candidateType") {
            if (!args.empty())
                unsupported("RayQuery." + name + " takes no arguments");
            // OpRayQueryGetIntersectionTypeKHR selector: 1 = committed,
            // 0 = candidate.
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
        if (name == "candidateDistance" || name == "committedDistance") {
            if (!args.empty())
                unsupported("RayQuery." + name + " takes no arguments");
            llvm::Value* sel = llvm::ConstantInt::get(
                i32, name == "committedDistance" ? 1 : 0);
            return target.rayQueryIntersectionT(builder, mod, rqPtr, sel);
        }
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
    // materialized descriptor; like a sampler it must be a bare identifier
    // naming an AccelerationStructure kernel param.
    llvm::Value* resolveAccelArg(const ExpressionPtr& e) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(e)) {
            auto it = accelHandles.find(id->getTextValue());
            if (it != accelHandles.end()) return it->second;
        }
        unsupported("RayQuery.initialize: first argument must be an "
                    "AccelerationStructure kernel parameter");
    }

    // Infer each Tile<T,R,C> local's SPIR-V Use from its position in the tile
    // multiply-accumulate: accumulator in Group.mac(acc, a, b), then MatrixA and
    // MatrixB. Runs before the tier scan and slot construction, which need it.
    void inferTileUses(const AbstractSyntaxNodePtr& root) {
        std::set<std::string> tileNames;
        std::function<void(const AbstractSyntaxNodePtr&)> collect =
            [&](const AbstractSyntaxNodePtr& node) {
                if (!node) return;
                if (auto lvd =
                        std::dynamic_pointer_cast<LocalVariableDeclaration>(node)) {
                    if (isTileType(lvd->getType())) {
                        for (auto& vd : lvd->getVariableDeclarators())
                            if (vd) tileNames.insert(vd->getIdentifier());
                    }
                }
                node->forEachSubNode(collect);
            };
        collect(root);
        if (tileNames.empty()) return;

        auto assign = [&](const ExpressionPtr& e, uint32_t use) {
            auto id = std::dynamic_pointer_cast<IdentifierExpression>(e);
            if (!id) return;
            const std::string& nm = id->getTextValue();
            if (!tileNames.count(nm)) return;
            tileInferredUse.emplace(nm, use);   // first wins
        };
        std::function<void(const AbstractSyntaxNodePtr&)> walk =
            [&](const AbstractSyntaxNodePtr& node) {
                if (!node) return;
                if (auto mc =
                        std::dynamic_pointer_cast<MethodCallExpression>(node)) {
                    const std::string& mn = mc->getMethodCallName();
                    const auto& ps = mc->getParameters();
                    if (mn == "mac" && ps.size() == 3) {
                        // Group.mac(acc, a, b)
                        assign(ps[0].expression, 2);
                        assign(ps[1].expression, 0);
                        assign(ps[2].expression, 1);
                    } else if (mn == "mma" && ps.size() == 2) {
                        if (auto recvId = macReceiverId(mc)) assign(recvId, 2);
                        assign(ps[0].expression, 0);
                        assign(ps[1].expression, 1);
                    }
                }
                node->forEachSubNode(walk);
            };
        walk(root);
    }

    // The accumulator identifier of a bare `acc.mma(a, b)` written on a Tile.
    // The receiver is children[0]; arguments live in getParameters().
    ExpressionPtr macReceiverId(
            const std::shared_ptr<MethodCallExpression>& mc) {
        if (mc->getChildren().empty()) return nullptr;
        return std::dynamic_pointer_cast<IdentifierExpression>(
            mc->getChildren()[0]);
    }

    // Decide the cooperative-matrix tier for the KERNEL rather than per tile:
    // walk the body's tile declarations, ask the target for each base tier, and
    // record whether they straddle. Must run before any slot is built.
    void scanCoopMatrixTiers(const AbstractSyntaxNodePtr& root) {
        bool anyNative = false;
        bool anyPortable = false;
        bool anyEpilogueVerb = false;
        std::function<void(const AbstractSyntaxNodePtr&)> walk =
            [&](const AbstractSyntaxNodePtr& node) {
                if (!node) return;
                if (auto vmc =
                        std::dynamic_pointer_cast<MethodCallExpression>(node)) {
                    const std::string& mn = vmc->getMethodCallName();
                    if (mn == "scaledAccumInto" || mn == "rank1Accum" ||
                        mn == "scaledAccumInto2" ||
                        mn == "scaledAccumIntoS" ||
                        mn == "scaledAccumInto2S")
                        anyEpilogueVerb = true;
                }
                if (auto lvd =
                        std::dynamic_pointer_cast<LocalVariableDeclaration>(node)) {
                    CajetaTypePtr dt = lvd->getType();
                    bool tile = isTileType(dt);
                    if (isCooperativeMatrixType(dt) || tile) {
                        if (auto cm = std::dynamic_pointer_cast<CajetaClass>(dt)) {
                            const auto& ta = cm->getTypeArguments();
                            // CooperativeMatrix<T,R,C,Use> carries Use in the
                            // type; Tile<T,R,C> has it inferred per declarator.
                            if (ta.size() == (tile ? 3u : 4u)) {
                                llvm::Type* el = deviceScalarType(ta[0], ctx);
                                auto r = std::dynamic_pointer_cast<CajetaConstantType>(ta[1]);
                                auto c = std::dynamic_pointer_cast<CajetaConstantType>(ta[2]);
                                auto u = tile ? nullptr
                                    : std::dynamic_pointer_cast<CajetaConstantType>(ta[3]);
                                for (auto& vd : lvd->getVariableDeclarators()) {
                                    if (!vd) continue;
                                    uint32_t useVal;
                                    if (tile) {
                                        auto it = tileInferredUse.find(vd->getIdentifier());
                                        if (it == tileInferredUse.end()) continue;
                                        useVal = it->second;
                                    } else {
                                        if (!u) continue;
                                        useVal = (uint32_t) u->getValue();
                                    }
                                    if (el && r && c) {
                                        auto t = target.coopMatrixTier(
                                            el, (uint32_t) r->getValue(),
                                            (uint32_t) c->getValue(), useVal);
                                        if (t == LoweringTarget::ImplTier::Native)
                                            anyNative = true;
                                        else
                                            anyPortable = true;
                                    }
                                }
                            }
                        }
                    }
                }
                node->forEachSubNode(walk);
            };
        walk(root);
        coopStraddleDemote = anyNative && anyPortable;
        if (anyEpilogueVerb && anyNative &&
            !target.coopMatrixEpilogueSupported()) {
            coopStraddleDemote = true;
            std::string key = std::string("epilogue@") + target.name();
            if (notedCoopTiers.insert(key).second) {
                std::cerr << "note: [mma-epilogue] scaledAccumInto/"
                             "rank1Accum have no native lowering on the "
                          << target.name() << " backend - the kernel's "
                             "cooperative-matrix tiles take the portable "
                             "software tile (the result is identical; the "
                             "epilogue runs in the accumulator's registers "
                             "on backends that support it, e.g. AMD WMMA)."
                          << std::endl;
            }
        }
    }

    // Build a CooperativeMatrix/Tile local's slot from its declared type args,
    // picking the native or the software tier (and honouring a group demotion).
    CoopMatrixSlot buildCoopMatrixSlot(const CajetaTypePtr& declType,
                                       const std::string& nm) {
        bool tile = isTileType(declType);
        auto cls = std::dynamic_pointer_cast<CajetaClass>(declType);
        size_t wantArgs = tile ? 3 : 4;
        if (!cls || cls->getTypeArguments().size() != wantArgs)
            unsupported(tile ? "Tile requires <T, Rows, Cols>"
                             : "CooperativeMatrix requires <T, Rows, Cols, Use>");
        const auto& targs = cls->getTypeArguments();
        llvm::Type* elem = deviceScalarType(targs[0], ctx);
        if (!elem)
            unsupported((tile ? "Tile" : "CooperativeMatrix")
                        + std::string(" element type must be a numeric primitive"));
        auto rows = std::dynamic_pointer_cast<CajetaConstantType>(targs[1]);
        auto cols = std::dynamic_pointer_cast<CajetaConstantType>(targs[2]);
        if (!rows || !cols)
            unsupported((tile ? "Tile" : "CooperativeMatrix")
                        + std::string(" Rows/Cols must be integer constants"));
        uint32_t useVal;
        if (tile) {
            auto it = tileInferredUse.find(nm);
            if (it == tileInferredUse.end())
                unsupported("Tile '" + nm + "' has no inferred role — every "
                            "Tile must appear in a Group.mac(acc, a, b) so its "
                            "MatrixA / MatrixB / accumulator role is known (§4.1)");
            useVal = it->second;
        } else {
            auto use = std::dynamic_pointer_cast<CajetaConstantType>(targs[3]);
            if (!use)
                unsupported("CooperativeMatrix Use must be an integer constant");
            useVal = (uint32_t) use->getValue();
        }
        CoopMatrixSlot s;
        s.elemType = elem;
        s.elemSigned = typeIsSigned(targs[0]);
        s.rows = (uint32_t) rows->getValue();
        s.cols = (uint32_t) cols->getValue();
        s.use  = useVal;
        auto baseTier = target.coopMatrixTier(elem, s.rows, s.cols, s.use);
        auto tier = resolveImplTier("COOPMATRIX", baseTier);
        bool straddled = false;
        if (coopStraddleDemote && tier == LoweringTarget::ImplTier::Native) {
            tier = LoweringTarget::ImplTier::Portable;
            straddled = true;
        }
        if (tier == LoweringTarget::ImplTier::Portable) {
            s.software = true;
            s.matrixType = llvm::ArrayType::get(elem, (uint64_t) s.rows * s.cols);
            s.alloca = entryAlloca(s.matrixType, nm);
            if (s.use == 0)
                noteSoftwareCoopMatrix(elem, s.rows, s.cols,
                                       baseTier == LoweringTarget::ImplTier::Native
                                           && !straddled);
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

    // Emit a sticky `note:` - a severity below `warning:` - that a
    // CooperativeMatrix took the portable software path on this backend. Worded
    // as a capability statement; emitted once per distinct (dtype, shape).
    void noteSoftwareCoopMatrix(llvm::Type* elem, uint32_t rows, uint32_t cols,
                                bool forced) {
        std::string dt = deviceScalarName(elem);
        std::string key = dt + ":" + std::to_string(rows) + "x" +
                          std::to_string(cols) + "@" + target.name();
        if (!notedCoopTiers.insert(key).second) return;
        if (forced) {
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

    // Emit `for (i32 iv = 0; iv < count; ++iv) body(iv)` for a compile-time
    // bound, so a software coop tile need not unroll Rows*Cols ops. body() is
    // emitted with the builder in the loop body; on return it is at the exit.
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
    // software tile can index it per element. Unlike resolveBufferTileArg this
    // returns the base, not a pre-offset pointer.
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

    // Resolve an epilogue-verb Shared argument that may be a SLICE: `arr[expr]`
    // gives the address of element `expr`, so per-wave scale vectors can share
    // one array. A bare identifier still resolves to the array base.
    bool resolveBufferBaseOrSlice(const ExpressionPtr& e,
                                  llvm::Value*& base, llvm::Type*& elemTy) {
        if (resolveBufferBase(e, base, elemTy)) return true;
        if (auto ai = std::dynamic_pointer_cast<ArrayIndexExpression>(e)) {
            llvm::Value* b = nullptr; llvm::Type* et = nullptr;
            if (resolveBufferBase(exprChild(ai, 0), b, et) && et) {
                llvm::Value* idx = coerceTo(
                    lowerExpr(exprChild(ai, 1)),
                    llvm::Type::getInt64Ty(ctx));
                base = target.bufferElementPtr(builder, mod, b, et, idx);
                elemTy = et;
                return true;
            }
        }
        return false;
    }

    // CoopStage.panel(dst, src, rowBase, colBase, rows, cols, ld): the
    // workgroup-cooperative global-to-LDS staging copy, every thread striding
    // over the panel. The caller owns the surrounding Barrier.workgroup() calls.
    llvm::Value* lowerCoopStage(const std::string& name,
                                const std::shared_ptr<MethodCallExpression>& mc) {
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
        if (name == "panelTransposed") {
            const auto& ta = mc->getParameters();
            if (ta.size() != 8)
                unsupported("CoopStage.panelTransposed expects (Shared dst, Buffer src, "
                            "rowBase, colBase, rows, cols, ld, pad)");
            llvm::Value* dB = nullptr; llvm::Type* dE = nullptr;
            llvm::Value* sB = nullptr; llvm::Type* sE = nullptr;
            if (!resolveBufferBase(ta[0].expression, dB, dE))
                unsupported("CoopStage.panelTransposed: dst must be a Shared<T> local");
            if (!resolveBufferBase(ta[1].expression, sB, sE))
                unsupported("CoopStage.panelTransposed: src must be a Buffer<T> param");
            llvm::Value* rB = coerceTo(lowerExpr(ta[2].expression), i32);
            llvm::Value* cB = coerceTo(lowerExpr(ta[3].expression), i32);
            llvm::Value* rws = coerceTo(lowerExpr(ta[4].expression), i32);
            llvm::Value* cls = coerceTo(lowerExpr(ta[5].expression), i32);
            llvm::Value* ldv = coerceTo(lowerExpr(ta[6].expression), i32);
            llvm::Value* pdv = coerceTo(lowerExpr(ta[7].expression), i32);
            unsigned eb = sE->getPrimitiveSizeInBits();
            unsigned lanes = eb ? (128u / eb) : 1u;
            if (lanes < 1) lanes = 1;
            llvm::Value* lanesV = llvm::ConstantInt::get(i32, lanes);
            llvm::Value* kpad = builder.CreateAdd(rws, pdv, "bt.kpad");
            llvm::Value* cpr = builder.CreateUDiv(cls, lanesV, "bt.cpr");
            llvm::Value* tot = builder.CreateMul(rws, cpr, "bt.total");
            llvm::Value* tid = coerceTo(target.threadId(builder, mod, 0), i32);
            llvm::Value* nth = coerceTo(target.workgroupDim(builder, mod, 0), i32);
            llvm::Value* iv = entryAlloca(i32, "bt.e");
            builder.CreateStore(tid, iv);
            auto* h = llvm::BasicBlock::Create(ctx, "bt.head", fn);
            auto* bd = llvm::BasicBlock::Create(ctx, "bt.body", fn);
            auto* ex = llvm::BasicBlock::Create(ctx, "bt.exit", fn);
            builder.CreateBr(h);
            builder.SetInsertPoint(h);
            llvm::Value* e = builder.CreateLoad(i32, iv, "bt.e.cur");
            builder.CreateCondBr(builder.CreateICmpULT(e, tot), bd, ex);
            builder.SetInsertPoint(bd);
            llvm::Value* r = builder.CreateUDiv(e, cpr);
            llvm::Value* cc = builder.CreateMul(builder.CreateURem(e, cpr), lanesV);
            llvm::Value* gIdx = builder.CreateAdd(
                builder.CreateMul(builder.CreateAdd(rB, r), ldv),
                builder.CreateAdd(cB, cc), "bt.gidx");
            llvm::Value* vec = target.vectorLoad(
                builder, mod, sB, sE, lanes, builder.CreateZExt(gIdx, i64));
            for (unsigned i = 0; i < lanes; ++i) {
                llvm::Value* ci = builder.CreateAdd(cc, llvm::ConstantInt::get(i32, i));
                llvm::Value* dIdx = builder.CreateAdd(
                    builder.CreateMul(ci, kpad), r, "bt.didx");
                llvm::Value* dPtr = target.bufferElementPtr(
                    builder, mod, dB, dE, builder.CreateZExt(dIdx, i64));
                llvm::Value* elt = builder.CreateExtractElement(vec, i);
                builder.CreateStore(coerceTo(elt, dE), dPtr);
            }
            builder.CreateStore(builder.CreateAdd(e, nth), iv);
            builder.CreateBr(h);
            builder.SetInsertPoint(ex);
            return llvm::ConstantInt::get(i32, 0);
        }
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
        llvm::Value* dIdx = maybeSwizzle(dstBase, builder.CreateZExt(e, i64));
        llvm::Value* dPtr = target.bufferElementPtr(
            builder, mod, dstBase, dstElem, dIdx);
        builder.CreateStore(coerceTo(val, dstElem), dPtr);
        builder.CreateStore(builder.CreateAdd(e, nthr), iv);
        builder.CreateBr(head);
        builder.SetInsertPoint(exit);
        return llvm::ConstantInt::get(i32, 0);
    }

    // A synchronous workgroup-strided global-to-LDS copy whose dst index runs
    // through the tile's swizzle: dst[swizzle(dstOffset+e)] = src[srcOffset+e].
    // Used for AsyncCopy.copy into a Swizzled tile - async cannot permute.
    void emitSwizzledSyncCopy(llvm::Value* dstBase, llvm::Type* dstElem,
                              llvm::Value* dstOffset, llvm::Value* srcBase,
                              llvm::Type* srcElem, llvm::Value* srcOffset,
                              llvm::Value* count) {
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
        llvm::Value* tid  = coerceTo(target.threadId(builder, mod, 0), i32);
        llvm::Value* nthr = coerceTo(target.workgroupDim(builder, mod, 0), i32);
        llvm::Value* iv = entryAlloca(i32, "swzcopy.e");
        builder.CreateStore(tid, iv);
        auto* head = llvm::BasicBlock::Create(ctx, "swzcopy.head", fn);
        auto* body = llvm::BasicBlock::Create(ctx, "swzcopy.body", fn);
        auto* exit = llvm::BasicBlock::Create(ctx, "swzcopy.exit", fn);
        builder.CreateBr(head);
        builder.SetInsertPoint(head);
        llvm::Value* e = builder.CreateLoad(i32, iv, "swzcopy.e.cur");
        builder.CreateCondBr(builder.CreateICmpULT(e, count), body, exit);
        builder.SetInsertPoint(body);
        llvm::Value* sPtr = target.bufferElementPtr(
            builder, mod, srcBase, srcElem,
            builder.CreateZExt(builder.CreateAdd(srcOffset, e), i64));
        llvm::Value* val = builder.CreateLoad(srcElem, sPtr, "swzcopy.ld");
        llvm::Value* dIdx = maybeSwizzle(
            dstBase, builder.CreateZExt(builder.CreateAdd(dstOffset, e), i64));
        llvm::Value* dPtr =
            target.bufferElementPtr(builder, mod, dstBase, dstElem, dIdx);
        builder.CreateStore(coerceTo(val, dstElem), dPtr);
        builder.CreateStore(builder.CreateAdd(e, nthr), iv);
        builder.CreateBr(head);
        builder.SetInsertPoint(exit);
    }

    // AsyncCopy.copy/commit/wait: async global-to-LDS transfers plus the group
    // commit/wait of an N-stage software prefetch, through the async seams.
    llvm::Value* lowerAsyncCopy(const std::string& name,
                                const std::shared_ptr<MethodCallExpression>& mc) {
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        const auto& args = mc->getParameters();
        if (name == "commit") {
            if (!args.empty())
                unsupported("AsyncCopy.commit expects no arguments");
            target.asyncCommit(builder, mod);
            return llvm::ConstantInt::get(i32, 0);
        }
        if (name == "wait") {
            if (args.size() != 1)
                unsupported("AsyncCopy.wait expects (groupsInFlight)");
            target.asyncWait(builder, mod,
                             coerceTo(lowerExpr(args[0].expression), i32));
            return llvm::ConstantInt::get(i32, 0);
        }
        if (name != "copy")
            unsupported("AsyncCopy." + name + "()");
        if (args.size() != 5)
            unsupported("AsyncCopy.copy expects (Shared dst, dstOffset, "
                        "Buffer src, srcOffset, count)");
        llvm::Value* dstBase = nullptr; llvm::Type* dstElem = nullptr;
        llvm::Value* srcBase = nullptr; llvm::Type* srcElem = nullptr;
        if (!resolveBufferBase(args[0].expression, dstBase, dstElem))
            unsupported("AsyncCopy.copy: dst must be a Shared<T> kernel local");
        if (!resolveBufferBase(args[2].expression, srcBase, srcElem))
            unsupported("AsyncCopy.copy: src must be a Buffer<T> kernel parameter");
        llvm::Value* dstOffset = coerceTo(lowerExpr(args[1].expression), i32);
        llvm::Value* srcOffset = coerceTo(lowerExpr(args[3].expression), i32);
        llvm::Value* count     = coerceTo(lowerExpr(args[4].expression), i32);
        if (swizzledBaseStride.count(dstBase)) {
            emitSwizzledSyncCopy(dstBase, dstElem, dstOffset,
                                 srcBase, srcElem, srcOffset, count);
            return llvm::ConstantInt::get(i32, 0);
        }
        target.asyncCopy(builder, mod, dstBase, dstElem, dstOffset,
                         srcBase, srcElem, srcOffset, count);
        return llvm::ConstantInt::get(i32, 0);
    }

    // Schedule.barrier/groupBarrier/priority/pipelineOpt instruction-scheduling
    // hints. Every operand is an ImmArg, so a non-constant or out-of-range value
    // is a call-site diagnostic here, not an LLVM verifier crash.
    llvm::Value* lowerSchedule(const std::string& name,
                               const std::shared_ptr<MethodCallExpression>& mc) {
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        const auto& args = mc->getParameters();
        auto constArg = [&](size_t i, const char* what) -> uint64_t {
            auto* ci = llvm::dyn_cast<llvm::ConstantInt>(
                lowerExpr(args[i].expression));
            if (!ci)
                unsupported("Schedule." + name + ": " + what +
                            " must be a compile-time constant");
            return ci->getZExtValue();
        };
        if (name == "barrier") {
            if (args.size() != 1)
                unsupported("Schedule.barrier expects (mask)");
            target.schedBarrier(builder, mod, (uint32_t) constArg(0, "mask"));
        } else if (name == "groupBarrier") {
            if (args.size() != 3)
                unsupported("Schedule.groupBarrier expects (mask, size, syncId)");
            target.schedGroupBarrier(builder, mod, (uint32_t) constArg(0, "mask"),
                                     (uint32_t) constArg(1, "size"),
                                     (uint32_t) constArg(2, "syncId"));
        } else if (name == "priority") {
            if (args.size() != 1)
                unsupported("Schedule.priority expects (level)");
            uint64_t level = constArg(0, "level");
            if (level > 3)
                unsupported("Schedule.priority: level must be 0..3");
            target.schedPriority(builder, mod, (uint32_t) level);
        } else if (name == "pipelineOpt") {
            if (args.size() != 1)
                unsupported("Schedule.pipelineOpt expects (strategy)");
            uint64_t strategy = constArg(0, "strategy");
            if (strategy > 1)
                unsupported("Schedule.pipelineOpt: unknown strategy (0=GEMM, "
                            "1=MFMA-exp)");
            target.schedPipelineOpt(builder, mod, (uint32_t) strategy);
        } else {
            unsupported("Schedule." + name + "()");
        }
        return llvm::ConstantInt::get(i32, 0);
    }

    // Resolve a CooperativeMatrix.load/store Buffer argument to a device pointer
    // at element `offset` - the base the tile reads or writes Rows*Cols elements
    // from. `offset` selects a sub-tile; 0 is the whole-buffer base.
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

    // CooperativeMatrix op dispatch: the slot alloca holds the opaque tile.
    // load/splat/mma write it; store/mma read it, through the backend seams.
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
            uint32_t swz = swizzleStrideOfArg(args[0].expression);
            llvm::Value* offset = lowerExpr(args[1].expression);
            llvm::Value* tileBase = nullptr;
            auto bp = blockPadOfArg(args[0].expression, &tileBase);
            LdsBlockPad blk;
            llvm::Value* ptr;
            if (bp.first) {
                ptr = tileBase;
                blk.period = bp.first;
                blk.pad = bp.second;
                blk.baseOffset =
                    builder.CreateZExtOrTrunc(offset, llvm::Type::getInt32Ty(ctx));
            } else {
                ptr = resolveBufferTileArg(args[0].expression, offset);
            }
            llvm::Value* layout = coerceTo(lowerExpr(args[2].expression), i32);
            llvm::Value* stride = coerceTo(lowerExpr(args[3].expression), i32);
            if (name == "load") {
                llvm::Value* v = target.coopMatrixLoad(
                    builder, mod, ptr, layout, stride, slot.matrixType,
                    slot.rows, slot.cols, slot.use, swz, blk);
                builder.CreateStore(v, slot.alloca);
            } else {
                llvm::Value* v = builder.CreateLoad(slot.matrixType, slot.alloca,
                                                    recv + ".val");
                target.coopMatrixStore(builder, mod, ptr, v, layout, stride,
                                       slot.rows, slot.cols, slot.use, swz, blk);
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
        if (name == "fromWords") {
            if (args.size() != 4)
                unsupported("CooperativeMatrix.fromWords expects "
                            "(w0, w1, w2, w3)");
            if (!target.coopMatrixFromWordsSupported())
                unsupported("CooperativeMatrix.fromWords: NATIVE-ONLY on "
                            "backends with an explicit per-lane fragment "
                            "(AMD WMMA); the " + std::string(target.name()) +
                            " cooperative matrix is opaque");
            if (slot.use == 2)
                unsupported("CooperativeMatrix.fromWords: an A or B "
                            "operand tile, not an accumulator");
            auto* vt = llvm::dyn_cast<llvm::FixedVectorType>(slot.matrixType);
            if (!vt || vt->getNumElements() != 4 ||
                !vt->getElementType()->isIntegerTy(32))
                unsupported("CooperativeMatrix.fromWords: only the 16x16 "
                            "int8 operand fragment (<4 x i32> per lane)");
            llvm::Value* frag = llvm::UndefValue::get(vt);
            for (unsigned w = 0; w < 4; ++w) {
                llvm::Value* wv = coerceTo(lowerExpr(args[w].expression), i32);
                frag = builder.CreateInsertElement(
                    frag, wv, llvm::ConstantInt::get(i32, w),
                    recv + ".fw" + std::to_string(w));
            }
            builder.CreateStore(frag, slot.alloca);
            return llvm::ConstantInt::get(i32, 0);
        }
        if (name == "mma") {
            if (args.size() != 2)
                unsupported("CooperativeMatrix.mma expects (a, b)");
            CoopMatrixSlot a = resolveCoopMatrixArg(args[0].expression);
            CoopMatrixSlot b = resolveCoopMatrixArg(args[1].expression);
            if (a.software || b.software)
                unsupported("CooperativeMatrix.mma: a native accumulator cannot "
                            "consume software-tier operands Ã¢ÂÂ give all three "
                            "tiles the same dtype tier");
            llvm::Value* aVal = builder.CreateLoad(a.matrixType, a.alloca, "cm.a");
            llvm::Value* bVal = builder.CreateLoad(b.matrixType, b.alloca, "cm.b");
            llvm::Value* cVal =
                builder.CreateLoad(slot.matrixType, slot.alloca, "cm.c");
            // SPV_KHR_cooperative_matrix signedness mask: A=0x1 B=0x2 C=0x4
            // Result=0x8. SPIR-V integer types are SIGNLESS - omitting these
            // executes signed int8 as unsigned (-1 reads as 255).
            uint32_t signFlags = 0;
            if (a.elemSigned) signFlags |= 0x1u;
            if (b.elemSigned) signFlags |= 0x2u;
            if (slot.elemSigned) signFlags |= 0x4u | 0x8u;
            llvm::Value* v = target.coopMatrixMulAdd(builder, mod, aVal, bVal,
                                                     cVal, slot.matrixType,
                                                     signFlags);
            builder.CreateStore(v, slot.alloca);
            return llvm::ConstantInt::get(i32, 0);
        }
        if (name == "scaledAccumInto" || name == "rank1Accum" ||
            name == "scaledAccumInto2" || name == "scaledAccumIntoS" ||
            name == "scaledAccumInto2S") {
            const bool scaled = (name != "rank1Accum");
            const bool dual = (name == "scaledAccumInto2" ||
                               name == "scaledAccumInto2S");
            const bool scalarCol = (name == "scaledAccumIntoS" ||
                                    name == "scaledAccumInto2S");
            const size_t want = dual ? 5 : (scaled ? 3 : 2);
            if (args.size() != want)
                unsupported(std::string("CooperativeMatrix.") + name +
                            (dual ? " expects (facc, rowF, colF, rowG, colG)"
                                  : (scaled ? " expects (facc, rowF, colF)"
                                            : " expects (rowF, colF)")));
            if (!target.coopMatrixEpilogueSupported())
                unsupported(std::string("CooperativeMatrix.") + name +
                            ": no native epilogue lowering on this backend "
                            "(tier scan should have demoted)");
            CoopMatrixSlot facc = slot;
            if (scaled) {
                facc = resolveCoopMatrixArg(args[0].expression);
                if (facc.software)
                    unsupported("CooperativeMatrix.scaledAccumInto: facc "
                                "must share the receiver's (native) tier");
                if (facc.rows != slot.rows || facc.cols != slot.cols)
                    unsupported("CooperativeMatrix.scaledAccumInto: "
                                "receiver and facc must share Rows/Cols");
            }
            if (slot.use != 2 || facc.use != 2)
                unsupported(std::string("CooperativeMatrix.") + name +
                            ": accumulator tiles (Use=2) only");
            if (!facc.elemType->isFloatTy())
                unsupported(std::string("CooperativeMatrix.") + name +
                            ": the target accumulator must be float32");
            llvm::Value* rB = nullptr; llvm::Type* rE = nullptr;
            llvm::Value* cB = nullptr; llvm::Type* cE = nullptr;
            llvm::Value* gB = nullptr; llvm::Type* gE = nullptr;
            llvm::Value* hB = nullptr; llvm::Type* hE = nullptr;
            llvm::Value* cS = nullptr; llvm::Value* gS = nullptr;
            const size_t ri = scaled ? 1 : 0;
            if (!resolveBufferBaseOrSlice(args[ri].expression, rB, rE))
                unsupported(std::string("CooperativeMatrix.") + name +
                            ": rowF must be a Shared<float32> vector "
                            "(or a slice of one)");
            if (scalarCol) {
                cS = toFloat(lowerExpr(args[ri + 1].expression));
                if (dual) {
                    if (!resolveBufferBaseOrSlice(args[ri + 2].expression,
                                                  gB, gE))
                        unsupported("CooperativeMatrix.scaledAccumInto2S: "
                                    "rowG must be a Shared<float32> vector "
                                    "(or a slice of one)");
                    gS = toFloat(lowerExpr(args[ri + 3].expression));
                }
            } else {
                if (!resolveBufferBaseOrSlice(args[ri + 1].expression,
                                              cB, cE))
                    unsupported(std::string("CooperativeMatrix.") + name +
                                ": colF must be a Shared<float32> vector");
                if (dual &&
                    (!resolveBufferBaseOrSlice(args[ri + 2].expression,
                                               gB, gE) ||
                     !resolveBufferBaseOrSlice(args[ri + 3].expression,
                                               hB, hE)))
                    unsupported("CooperativeMatrix.scaledAccumInto2: "
                                "rowG/colG must be Shared<float32> vectors");
            }
            llvm::Value* accVal = nullptr;
            if (scaled)
                accVal = builder.CreateLoad(slot.matrixType, slot.alloca,
                                            recv + ".val");
            llvm::Value* faccVal =
                builder.CreateLoad(facc.matrixType, facc.alloca, "epi.facc");
            llvm::Value* v = target.coopMatrixEpilogueAccum(
                builder, mod, accVal, faccVal, rB, rE, cB,
                cE ? cE : rE, gB, hB, cS, gS);
            builder.CreateStore(v, facc.alloca);
            return llvm::ConstantInt::get(i32, 0);
        }
        unsupported("CooperativeMatrix." + name + "()");
    }

    // Software cooperative-matrix ops: the slot is a flat `[R*C x elem]` tile.
    // splat fills it, load/store gather/scatter it from a Buffer, and mma runs a
    // triple loop; FP GEMMs accumulate in f32 and narrow on store.
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
                    llvm::Value* bidx = maybeSwizzle(
                        base, builder.CreateZExt(builder.CreateAdd(offset, sel), i64));
                    llvm::Value* bptr =
                        target.bufferElementPtr(builder, mod, base, bElem, bidx);
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
                            "consume native-tier operands Ã¢ÂÂ give all three tiles "
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

        if (name == "fromWords") {
            unsupported("CooperativeMatrix.fromWords: NATIVE-ONLY (the "
                        "words are this lane's fragment row/column; the "
                        "software tile has no lane mapping). Stage the "
                        "widened bytes and `load` them on this tier");
        }
        if (name == "scaledAccumIntoS" || name == "scaledAccumInto2S") {
            unsupported(std::string("CooperativeMatrix.") + name +
                        ": NATIVE-ONLY (the scalar colF/colG is this "
                        "lane's column factor; the software tile has no "
                        "lane-column mapping). Use the Shared-vector "
                        "form " +
                        (name == "scaledAccumIntoS" ? "scaledAccumInto"
                                                    : "scaledAccumInto2") +
                        " on this tier");
        }
        // Element order and association are the CONTRACT every tier shares: one
        // fma chain per element, `(rowF[r] * colF[c]) * this[r][c]` then add.
        // The native lowerings must emit the same grouping - equality is exact.
        if (name == "scaledAccumInto" || name == "rank1Accum" ||
            name == "scaledAccumInto2") {
            const bool scaled = (name != "rank1Accum");
            const bool dual = (name == "scaledAccumInto2");
            const size_t want = dual ? 5 : (scaled ? 3 : 2);
            if (args.size() != want)
                unsupported(std::string("CooperativeMatrix.") + name +
                            (dual ? " expects (facc, rowF, colF, rowG, colG)"
                                  : (scaled ? " expects (facc, rowF, colF)"
                                            : " expects (rowF, colF)")));
            CoopMatrixSlot facc = slot;
            if (scaled) {
                facc = resolveCoopMatrixArg(args[0].expression);
                if (!facc.software)
                    unsupported("CooperativeMatrix.scaledAccumInto: the "
                                "float accumulator must share the "
                                "receiver's tier");
                if (facc.rows != R || facc.cols != C)
                    unsupported("CooperativeMatrix.scaledAccumInto: "
                                "receiver and facc must share Rows/Cols");
            }
            if (slot.use != 2 || facc.use != 2)
                unsupported(std::string("CooperativeMatrix.") + name +
                            ": accumulator tiles (Use=2) only");
            if (!facc.elemType->isFloatTy())
                unsupported(std::string("CooperativeMatrix.") + name +
                            ": the target accumulator must be float32");
            llvm::Value* rB = nullptr; llvm::Type* rE = nullptr;
            llvm::Value* cB = nullptr; llvm::Type* cE = nullptr;
            llvm::Value* gB = nullptr; llvm::Type* gE = nullptr;
            llvm::Value* hB = nullptr; llvm::Type* hE = nullptr;
            const size_t ri = scaled ? 1 : 0;
            if (!resolveBufferBaseOrSlice(args[ri].expression, rB, rE) ||
                !resolveBufferBaseOrSlice(args[ri + 1].expression, cB, cE))
                unsupported(std::string("CooperativeMatrix.") + name +
                            ": rowF/colF must be Shared<float32> vectors");
            if (dual &&
                (!resolveBufferBaseOrSlice(args[ri + 2].expression, gB, gE) ||
                 !resolveBufferBaseOrSlice(args[ri + 3].expression, hB, hE)))
                unsupported("CooperativeMatrix.scaledAccumInto2: "
                            "rowG/colG must be Shared<float32> vectors");
            emitCountedLoop(R, [&](llvm::Value* r) {
                llvm::Value* rv = builder.CreateLoad(
                    rE, target.bufferElementPtr(
                            builder, mod, rB, rE,
                            builder.CreateZExt(r, i64)), "epi.rf");
                emitCountedLoop(C, [&](llvm::Value* c) {
                    llvm::Value* cv = builder.CreateLoad(
                        cE, target.bufferElementPtr(
                                builder, mod, cB, cE,
                                builder.CreateZExt(c, i64)), "epi.cf");
                    llvm::Value* lin = builder.CreateAdd(
                        builder.CreateMul(r, llvm::ConstantInt::get(i32, C)),
                        c);
                    llvm::Value* term = builder.CreateFMul(rv, cv);
                    if (scaled) {
                        // toFloat, not coerceTo: the receiver is the int32
                        // accumulator, so this must be sitofp, never a bitcast.
                        llvm::Value* mcv = toFloat(builder.CreateLoad(
                            elem, coopElemPtr(slot, lin)));
                        term = builder.CreateFMul(term, mcv);
                    }
                    if (dual) {
                        llvm::Value* rg = builder.CreateLoad(
                            gE, target.bufferElementPtr(
                                    builder, mod, gB, gE,
                                    builder.CreateZExt(r, i64)),
                            "epi.rg");
                        llvm::Value* cg = builder.CreateLoad(
                            hE, target.bufferElementPtr(
                                    builder, mod, hB, hE,
                                    builder.CreateZExt(c, i64)),
                            "epi.cg");
                        term = builder.CreateFAdd(
                            term, builder.CreateFMul(rg, cg));
                    }
                    llvm::Value* fptr = coopElemPtr(facc, lin);
                    llvm::Value* cur =
                        builder.CreateLoad(facc.elemType, fptr, "epi.cur");
                    builder.CreateStore(builder.CreateFAdd(cur, term), fptr);
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

    // Coerce a value to f32 Ã¢ÂÂ texture coords are floats, but an int expression
    // (e.g. a lane index used as a coordinate) is widened via signed conversion.
    llvm::Value* toFloat(llvm::Value* v) {
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        if (v->getType() == f32) return v;
        if (v->getType()->isFloatingPointTy())
            return builder.CreateFPCast(v, f32);
        return builder.CreateSIToFP(v, f32);
    }

    // Math.<fn>(...) inside a kernel. Only the subset that lowers natively on
    // every backend with no device math-library link is admitted here; the rest
    // gets a clean diagnostic. Operates in the argument's FP type, not in f64.
    llvm::Value* lowerMathCall(const std::string& name,
                               const std::shared_ptr<MethodCallExpression>& mc) {
        const auto& args = mc->getParameters();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        auto asFp = [&](llvm::Value* v) -> llvm::Value* {
            llvm::Type* ty = v->getType();
            if (ty->isFPOrFPVectorTy()) return v;
            llvm::Type* target = f32;
            if (auto* vt = llvm::dyn_cast<llvm::FixedVectorType>(ty))
                target = llvm::FixedVectorType::get(f32, vt->getNumElements());
            return builder.CreateSIToFP(v, target);       // int -> f32
        };
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
            // Pick signed vs unsigned min/max by the operands' signedness - smin
            // on unsigned values is wrong, e.g. umin(0xFFFFFFFF, 1) must be 1.
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

    // Resolve `name(args)` / `Cls.name(args)` to a @Device method by name and
    // arity. Unqualified and `Self.helper(...)` resolve in the kernel's own
    // class; `OtherClass.helper(...)` resolves cross-class via the canonicalMap.
    MethodPtr resolveDeviceMethod(
            const std::string& recv, const std::string& name,
            const std::shared_ptr<MethodCallExpression>& mc) {
        if (!deviceFns) return nullptr;
        unsigned argc = (unsigned) mc->getParameters().size();
        auto findIn = [&](const std::shared_ptr<CajetaClass>& c) -> MethodPtr {
            if (!c) return nullptr;
            for (auto& kv : c->getMethods()) {
                const MethodPtr& m = kv.second;
                if (m && m->getName() == name && isDevice(*m) &&
                    m->getParameters().size() == argc)
                    return m;
            }
            return nullptr;
        };
        std::string simpleSelf;
        if (cls) {
            std::string q = cls->toCanonical();
            simpleSelf = q.substr(q.find_last_of('.') + 1);
            if (recv.empty() || recv == simpleSelf) {
                if (auto m = findIn(cls)) return m;
                if (recv.empty()) return nullptr;  // unqualified Ã¢ÂÂ same-class only
            }
        }
        if (recv.empty()) return nullptr;
        for (auto& kv : CajetaType::getCanonicalMap()) {
            auto c = std::dynamic_pointer_cast<CajetaClass>(kv.second);
            if (!c) continue;
            std::string canon = c->toCanonical();
            std::string simple = canon.substr(canon.find_last_of('.') + 1);
            if ((simple == recv || canon == recv) && simple != simpleSelf)
                if (auto m = findIn(c)) return m;
        }
        return nullptr;
    }

    // Lower a @Device method to a device function (scalar params + scalar/void
    // return), cached. alwaysinline so every backend folds the call away. A
    // nullptr cache entry means it's mid-lowering Ã¢ÂÂ a recursive call (rejected).
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
            tys.push_back(p.isBuffer ? target.bufferParamType(mod, p.type)
                                     : p.type);
        }
        llvm::Type* retTy = nullptr;
        if (auto rt = m->getReturnType()) {
            retTy = deviceScalarType(rt, ctx);
            if (!retTy) retTy = deviceVectorType(rt, ctx);     // Vector<T,N>-returning
            if (!retTy && rt->isValueType())
                retTy = deviceStructInfo(rt, ctx).type;
        }
        if (!retTy) retTy = llvm::Type::getVoidTy(ctx);
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
        sub.setParamsAsArgs(true);   // helper params are plain fn args
        sub.setDeviceContext(owner, deviceFns);
        sub.lowerBody(m);

        (*deviceFns)[m.get()] = hfn;
        return hfn;
    }

    // ----- Stage 11: bounded device-side dispatch -----

    // A @Device-static candidate must have no receiver on device.
    MethodPtr validateCallableCandidate(const MethodPtr& m) {
        if (!m->isStatic())
            unsupported("device callable candidate '" + m->getName() +
                        "' must be static (a device candidate has no receiver)");
        return m;
    }

    // Resolve a `Type::method` device-callable candidate to its @Device static
    // method by name and arity: the receiver's NAME is extracted and its class
    // resolved through the canonicalMap. An unnameable receiver is rejected.
    MethodPtr resolveCallableCandidate(
            const std::shared_ptr<MethodReferenceExpression>& mr, unsigned arity) {
        if (mr->getIsCtor())
            unsupported("device callable: a constructor reference (Type::heap) is "
                        "not a device candidate");
        std::string recvName;
        if (auto rt = mr->getReceiverType()) {
            if (rt->getQName()) recvName = rt->getQName()->getTypeName();
        }
        if (recvName.empty()) {
            if (auto rid = std::dynamic_pointer_cast<IdentifierExpression>(
                    mr->getReceiverExpr()))
                recvName = rid->getTextValue();
        }
        if (recvName.empty())
            unsupported("device callable: candidate must be a Type::method "
                        "reference to a @Device static method (a bound instance "
                        "reference is not a device candidate)");
        for (auto& kv : CajetaType::getCanonicalMap()) {
            auto c = std::dynamic_pointer_cast<CajetaClass>(kv.second);
            if (!c) continue;
            std::string canon = c->toCanonical();
            std::string simple = canon.substr(canon.find_last_of('.') + 1);
            if (simple != recvName && canon != recvName) continue;
            for (auto& mkv : c->getMethods()) {
                const MethodPtr& m = mkv.second;
                if (m && m->getName() == mr->getMethodName() &&
                    m->getParameters().size() == arity && isDevice(*m))
                    return validateCallableCandidate(m);
            }
        }
        unsupported("device callable: no @Device static '" + recvName + "." +
                    mr->getMethodName() + "' taking " + std::to_string(arity) +
                    " parameter(s)");
        return nullptr;  // unreachable (unsupported throws)
    }

    // Register a function-typed device local; both surface forms feed the same
    // tag mechanism, and the initializer shape decides which:
    //   table `((T)->R)[] ops = {A::f, B::g}` vs variable `(T)->R op = A::f`.
    void lowerCallableDecl(const std::string& nm,
                           const CajetaFunctionTypePtr& fnT,
                           const InitializerPtr& init) {
        DeviceCallable c;
        c.sig = fnT;
        unsigned arity = (unsigned) fnT->getParameterTypes().size();
        if (!init)
            unsupported("function-typed local '" + nm + "' needs an initializer "
                        "(a @Device-static dispatch set)");
        if (auto arrInit = std::dynamic_pointer_cast<ArrayInitializer>(init)) {
            c.isTable = true;
            for (auto& child : arrInit->getChildren()) {
                ExpressionPtr e;
                if (auto vi = std::dynamic_pointer_cast<VariableInitializer>(child)) {
                    if (!vi->getChildren().empty())
                        e = std::dynamic_pointer_cast<Expression>(
                            vi->getChildren()[0]);
                }
                auto mr = std::dynamic_pointer_cast<MethodReferenceExpression>(e);
                if (!mr)
                    unsupported("device dispatch table '" + nm + "' entries must "
                                "be Type::method references");
                c.candidates.push_back(resolveCallableCandidate(mr, arity));
            }
            if (c.candidates.empty())
                unsupported("device dispatch table '" + nm + "' is empty");
        } else {
            if (init->getChildren().empty())
                unsupported("function-typed local '" + nm + "' needs an "
                            "initializer (a @Device-static method reference)");
            auto e = std::dynamic_pointer_cast<Expression>(init->getChildren()[0]);
            if (auto mr =
                    std::dynamic_pointer_cast<MethodReferenceExpression>(e)) {
                c.candidates.push_back(resolveCallableCandidate(mr, arity));  // tag 0
                c.tagVal =
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
            } else {
                unsupported("function-typed local '" + nm + "': initializer must "
                            "be a @Device-static method reference (`Type::method`); "
                            "for runtime selection use a dispatch table "
                            "`((T)->R)[] ops = { A::f, B::g }` and index it");
            }
        }
        for (auto& m : c.candidates) lowerDeviceFn(m);
        callables[nm] = std::move(c);
    }

    // Lower a call through a bounded device callable as an if/else-if chain of
    // DIRECT calls keyed by the i32 tag - no indirect call, so it is SPIR-V-legal
    // everywhere. An unmatched tag returns the zero-initialized result slot.
    llvm::Value* emitCallableDispatch(
            const DeviceCallable& c, llvm::Value* tag,
            const std::vector<MethodCallParameter>& args) {
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        if (tag->getType() != i32)
            tag = builder.CreateIntCast(tag, i32, /*isSigned=*/false);
        std::vector<llvm::Value*> rawArgs;
        rawArgs.reserve(args.size());
        for (auto& a : args) rawArgs.push_back(lowerExpr(a.expression));
        llvm::Function* firstFn = lowerDeviceFn(c.candidates[0]);
        llvm::Type* retTy = firstFn->getReturnType();
        llvm::Value* resultSlot =
            retTy->isVoidTy() ? nullptr : entryAlloca(retTy, "dispatch.result");
        if (resultSlot)
            builder.CreateStore(llvm::Constant::getNullValue(retTy), resultSlot);
        llvm::Function* fn = builder.GetInsertBlock()->getParent();
        llvm::BasicBlock* contBB =
            llvm::BasicBlock::Create(ctx, "dispatch.cont", fn);
        for (size_t k = 0; k < c.candidates.size(); ++k) {
            llvm::Function* hfn = lowerDeviceFn(c.candidates[k]);
            llvm::BasicBlock* caseBB =
                llvm::BasicBlock::Create(ctx, "dispatch.case", fn);
            llvm::BasicBlock* nextBB =
                llvm::BasicBlock::Create(ctx, "dispatch.next", fn);
            llvm::Value* hit = builder.CreateICmpEQ(
                tag, llvm::ConstantInt::get(i32, k), "dispatch.is");
            builder.CreateCondBr(hit, caseBB, nextBB);
            builder.SetInsertPoint(caseBB);
            std::vector<llvm::Value*> argv;
            argv.reserve(rawArgs.size());
            for (unsigned i = 0; i < rawArgs.size() && i < hfn->arg_size(); ++i)
                argv.push_back(coerceTo(rawArgs[i], hfn->getArg(i)->getType()));
            llvm::Value* r = builder.CreateCall(
                hfn, argv, retTy->isVoidTy() ? "" : "dispatch.call");
            if (resultSlot) builder.CreateStore(r, resultSlot);
            builder.CreateBr(contBB);
            builder.SetInsertPoint(nextBB);
        }
        builder.CreateBr(contBB);
        builder.SetInsertPoint(contBB);
        if (resultSlot)
            return builder.CreateLoad(retTy, resultSlot, "dispatch.value");
        return llvm::ConstantInt::get(i32, 0);  // void call yields a dummy value
    }

    // The @ValueType of an operand expression: a bare value-type-typed name from
    // valueTypeNames, or the AST resolvedType when available. Null otherwise.
    CajetaTypePtr operandValueType(const ExpressionPtr& e) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(e)) {
            auto it = valueTypeNames.find(id->getTextValue());
            if (it != valueTypeNames.end()) return it->second;
        }
        return e ? e->getResolvedType() : nullptr;
    }

    // Route `a OP b` on a @ValueType LHS to the class's static @Device operator
    // (or a comparison derived from it), reusing the host's dispatch policy.
    // Returns nullptr when the LHS is not a value type, so the caller falls on.
    llvm::Value* lowerValueTypeBinaryOp(
            const std::shared_ptr<BinaryOpExpression>& bin,
            const ExpressionPtr& le, const ExpressionPtr& re, BinaryOp op) {
        CajetaTypePtr lhsType = operandValueType(le);
        if (!lhsType || !lhsType->isValueType()) return nullptr;
        auto lhsClass = std::dynamic_pointer_cast<CajetaClass>(lhsType);
        if (!lhsClass) return nullptr;
        if (!opdispatch::binaryOpSymbol(op)) return nullptr;
        CajetaTypePtr rhsType = operandValueType(re);
        if (!rhsType) rhsType = lhsType;   // homogeneous op is the common case
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
        // The LHS is a value type but no @Device operator applied: falling
        // through would emit an ICmp on the aggregate and crash.
        unsupported("no @Device 'operator" +
                    std::string(opdispatch::binaryOpSymbol(op)) +
                    "' for value type '" + lhsType->toCanonical() +
                    "' in kernel (declare it @Device, or the comparison it "
                    "derives from)");
    }

    // `*` on a matrix local: matmul (Matrix*Matrix, K checked), matVec
    // (Matrix*Vector) or scale (Matrix*scalar). Returns nullptr when the op is
    // not `*` or the LHS is not a matrix local, so the caller falls through.
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
        if (op == BINARY_OP_LOGAND || op == BINARY_OP_LOGOR)
            return lowerLogical(bin);
        ExpressionPtr le = exprChild(bin, 0), re = exprChild(bin, 1);
        if (llvm::Value* mm = lowerMatrixMul(le, re, op))
            return mm;
        if (llvm::Value* qm = lowerQuaternionMul(le, re, op))
            return qm;
        if (llvm::Value* vt = lowerValueTypeBinaryOp(bin, le, re, op))
            return vt;
        llvm::Value* l = lowerExpr(le);
        llvm::Value* r = lowerExpr(re);
        // The LHS drives the operation's signedness, so an unsigned `i >> 2`
        // stays a logical shift.
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

    // Core binary op on two lowered values, after width/fp unification. bfloat16
    // is a STORAGE format - native bfloat arithmetic is a vendor extension - so
    // it is computed in f32 and narrowed back; comparisons are not narrowed.
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

    // The byte value of a splat constant vector, or -1 when `v` is not one.
    static int splatByte(llvm::Value* v) {
        auto* c = llvm::dyn_cast<llvm::Constant>(v);
        if (!c) return -1;
        llvm::Constant* s = c->getSplatValue(/*AllowPoison=*/true);
        auto* ci = llvm::dyn_cast_or_null<llvm::ConstantInt>(s);
        if (!ci) return -1;
        return (int) (ci->getZExtValue() & 0xFF);
    }

    // `<N x i8>` viewed as `<N/4 x i32>` (N a multiple of 4), or nullptr.
    llvm::FixedVectorType* wordViewOf(llvm::Type* t) {
        auto* vt = llvm::dyn_cast<llvm::FixedVectorType>(t);
        if (!vt || !vt->getElementType()->isIntegerTy(8)) return nullptr;
        unsigned n = vt->getNumElements();
        if (n % 4 != 0) return nullptr;
        return llvm::FixedVectorType::get(llvm::Type::getInt32Ty(ctx), n / 4);
    }

    llvm::Value* wordMask(llvm::FixedVectorType* wt, unsigned byteMask) {
        uint32_t rep = byteMask * 0x01010101u;
        return llvm::ConstantVector::getSplat(
            wt->getElementCount(),
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), rep));
    }

    // True when CAJETA_XPU_NO_BYTE_WORDFORM=1 turns the byte-vector word-form
    // rewrites off: the A/B arm for measuring them with one compiler binary, and
    // the escape hatch if a backend ever mislowers the word view.
    bool byteWordFormOff() {
        static const bool off = [] {
            const char* e = std::getenv("CAJETA_XPU_NO_BYTE_WORDFORM");
            return e && *e && *e != '0';
        }();
        return off;
    }

    // Rewrite a byte-vector bitwise op into WORD form: amdgpu legalizes a
    // `<16 x i8>` and/or/shift one byte at a time, and the same bytes come out
    // of a `<4 x i32>` op. Returns nullptr when the shape is not one of these.
    llvm::Value* byteVectorWordForm(BinaryOp op, llvm::Value* l, llvm::Value* r,
                                    bool sign) {
        if (byteWordFormOff()) return nullptr;
        llvm::FixedVectorType* wt = wordViewOf(l->getType());
        if (!wt || r->getType() != l->getType()) return nullptr;
        // Capture the byte type HERE: the peephole below erases `l`, so reading
        // `l->getType()` after it is a use-after-free that came back as the WORD
        // type on device.
        llvm::Type* byteTy = l->getType();
        auto toWords = [&](llvm::Value* v) {
            return builder.CreateBitCast(v, wt, "bv.words");
        };
        auto toBytes = [&](llvm::Value* w) {
            return builder.CreateBitCast(w, byteTy, "bv.bytes");
        };
        auto shiftRightWords = [&](llvm::Value* w, int c) {
            if (c == 0) return w;
            return builder.CreateAnd(
                builder.CreateLShr(w, llvm::ConstantVector::getSplat(
                    wt->getElementCount(),
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), c))),
                wordMask(wt, 0xFFu >> c), "bv.lshr");
        };
        switch (op) {
            case BINARY_OP_BITAND: {
                // Peephole: on `(v >> c) & k` with k below bit 8-c, emit the
                // logical word form from the shift's operand and drop the ashr.
                int k = splatByte(r);
                auto* sh = llvm::dyn_cast<llvm::BinaryOperator>(l);
                if (sh && sh->getOpcode() == llvm::Instruction::AShr && k >= 0) {
                    int c = splatByte(sh->getOperand(1));
                    if (c >= 1 && c <= 7 && k < (1 << (8 - c))) {
                        llvm::Value* src = sh->getOperand(0);
                        if (sh->use_empty()) sh->eraseFromParent();
                        llvm::Value* w = shiftRightWords(toWords(src), c);
                        return toBytes(builder.CreateAnd(w, wordMask(wt, (unsigned) k),
                                                         "bv.and"));
                    }
                }
                return toBytes(builder.CreateAnd(toWords(l), toWords(r), "bv.and"));
            }
            case BINARY_OP_BITOR:
                return toBytes(builder.CreateOr(toWords(l), toWords(r), "bv.or"));
            case BINARY_OP_BITXOR:
                return toBytes(builder.CreateXor(toWords(l), toWords(r), "bv.xor"));
            case BINARY_OP_SHIFTLEFT: {
                int c = splatByte(r);
                if (c < 0 || c > 7) return nullptr;
                if (c == 0) return l;
                llvm::Value* w = builder.CreateShl(toWords(l), llvm::ConstantVector::getSplat(
                    wt->getElementCount(),
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), c)));
                return toBytes(builder.CreateAnd(w, wordMask(wt, (0xFFu << c) & 0xFFu),
                                                 "bv.shl"));
            }
            case BINARY_OP_SHIFTRIGHT:
                if (sign) return nullptr;       // ashr: per-byte (see above)
                [[fallthrough]];
            case BINARY_OP_USHIFTRIGHT: {
                int c = splatByte(r);
                if (c < 0 || c > 7) return nullptr;
                if (c == 0) return l;
                return toBytes(shiftRightWords(toWords(l), c));
            }
            default:
                return nullptr;
        }
    }

    llvm::Value* applyBinOpInner(BinaryOp op, llvm::Value* l, llvm::Value* r,
                                 bool sign, bool /*fpHint*/) {
        // isFPOrFPVectorTy: a bare isFloatingPointTy is false for a vector type.
        bool fp = l->getType()->isFPOrFPVectorTy();
        if (llvm::Value* w = byteVectorWordForm(op, l, r, sign)) return w;
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

    // Bring two scalars to a common type: fp dominates int (intÃ¢ÂÂfp); among
    // ints, extend the narrower to the wider (sext if signed, else zext).
    void unifyOperands(llvm::Value*& l, llvm::Value*& r, bool sign) {
        llvm::Type* lt = l->getType();
        llvm::Type* rt = r->getType();
        if (lt == rt) return;
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
        // Test FP-ness and integer width on the SCALAR element type, so vectors
        // of differing element types unify instead of tripping on a vector type.
        bool lFp = lt->getScalarType()->isFloatingPointTy();
        bool rFp = rt->getScalarType()->isFloatingPointTy();
        if (lFp || rFp) {
            llvm::Type* fT = lFp ? lt : rt;
            if (!lFp)        l = sign ? builder.CreateSIToFP(l, fT) : builder.CreateUIToFP(l, fT);
            else if (lt != fT) l = builder.CreateFPCast(l, fT);
            if (!rFp)        r = sign ? builder.CreateSIToFP(r, fT) : builder.CreateUIToFP(r, fT);
            else if (rt != fT) r = builder.CreateFPCast(r, fT);
            return;
        }
        llvm::Type* wide =
            lt->getScalarSizeInBits() >= rt->getScalarSizeInBits() ? lt : rt;
        l = builder.CreateIntCast(l, wide, sign);
        r = builder.CreateIntCast(r, wide, sign);
    }

    // ---- helpers --------------------------------------------------------

    ExpressionPtr exprChild(const std::shared_ptr<Expression>& e, size_t i) {
        if (e->getChildren().size() <= i) unsupported("missing operand");
        return std::dynamic_pointer_cast<Expression>(e->getChildren()[i]);
    }

    bool exprSigned(const ExpressionPtr& e) {
        if (!e) return true;
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
        // `buf.vload<N>(i)` carries the BUFFER's element signedness; without it a
        // chained call falls to the unknown-leaf default of signed.
        if (auto mc = std::dynamic_pointer_cast<MethodCallExpression>(e)) {
            const std::string& mn = mc->getMethodCallName();
            if (mn == "asUnsigned") return false;
            if (mn == "asSigned") return true;
            if ((mn == "widenLo" || mn == "widenHi" || mn == "narrow")
                    && !mc->getChildren().empty()) {
                return exprSigned(std::dynamic_pointer_cast<Expression>(
                    mc->getChildren()[0]));
            }
            if (mc->getMethodCallName() == "vload"
                    && !mc->getChildren().empty()) {
                if (auto b = std::dynamic_pointer_cast<IdentifierExpression>(
                        mc->getChildren()[0])) {
                    auto it = bufferElemSigned.find(b->getTextValue());
                    if (it != bufferElemSigned.end()) return it->second;
                }
            }
        }
        // Composite forms: a computed value is signed if either operand is, else
        // `(a-b) < 0` lowers to an always-false ICmpULT and `sum >> k` to LShr.
        if (auto bin = std::dynamic_pointer_cast<BinaryOpExpression>(e))
            return exprSigned(exprChild(bin, 0)) || exprSigned(exprChild(bin, 1));
        if (auto pre = std::dynamic_pointer_cast<PrefixExpression>(e))
            return exprSigned(exprChild(pre, 0));
        // Unknown leaf (cast, call, ternary): signed is the language default.
        return true;
    }

    // Coerce `v` to `ty`. `isSigned` governs integer WIDENING only (sign- vs
    // zero-extend) and is the SOURCE value's signedness; narrowing and
    // same-width casts ignore it. The default suits index/config coercions.
    llvm::Value* coerceTo(llvm::Value* v, llvm::Type* ty, bool isSigned = true) {
        if (v->getType() == ty) return v;
        if (v->getType()->isFloatingPointTy() && ty->isFloatingPointTy())
            return builder.CreateFPCast(v, ty);
        if (v->getType()->isIntegerTy() && ty->isIntegerTy())
            return builder.CreateIntCast(v, ty, isSigned);
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
// NVPTX and AMDGPU inherit these unchanged; Vulkan (SpirvTarget) overrides them.

llvm::Function* LoweringTarget::createKernel(
    llvm::Module& m, const std::string& name,
    const std::vector<KernelParam>& params) {
    llvm::LLVMContext& ctx = m.getContext();
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
    // addrspace-preserving GEP: the base pointer carries its address space
    // (1 for global buffers, 3 for shared globals).
    return b.CreateGEP(elemTy, base, {index}, "idx");
}

llvm::Value* LoweringTarget::vectorLoad(llvm::IRBuilderBase& b, llvm::Module& m,
                                        llvm::Value* base, llvm::Type* elemTy,
                                        unsigned lanes, llvm::Value* index) {
    // Packed contiguous load of `lanes` elements from element `index`, through
    // the same per-backend addressing seam as a scalar subscript. The base is
    // element-aligned. Vulkan overrides to split for the 4-component cap.
    llvm::Value* ptr = bufferElementPtr(b, m, base, elemTy, index);
    auto* vecTy = llvm::FixedVectorType::get(elemTy, lanes);
    auto* ld = b.CreateLoad(vecTy, ptr, "vload");
    ld->setAlignment(m.getDataLayout().getABITypeAlign(elemTy));
    return ld;
}

void LoweringTarget::vectorStore(llvm::IRBuilderBase& b, llvm::Module& m,
                                 llvm::Value* base, llvm::Type* elemTy,
                                 unsigned /*lanes*/, llvm::Value* index,
                                 llvm::Value* value) {
    // Symmetric packed store of a `<N x T>` value to `lanes` contiguous elements
    // from element `index`. N is implicit in `value`'s vector type.
    llvm::Value* ptr = bufferElementPtr(b, m, base, elemTy, index);
    auto* st = b.CreateStore(value, ptr);
    st->setAlignment(m.getDataLayout().getABITypeAlign(elemTy));
}

llvm::Value* LoweringTarget::bufferArrayElement(llvm::IRBuilderBase& b,
                                                llvm::Module& /*m*/,
                                                llvm::Function* /*fn*/,
                                                unsigned /*binding*/,
                                                llvm::Value* arrayBase,
                                                llvm::Type* /*elemTy*/,
                                                llvm::Value* descIndex) {
    // Pointer backends: `arrayBase` points at the [i64 count, i64 h0 ...] handle
    // array the launch marshalled, so bufs[idx] is handle 1+idx reinterpreted as
    // a device pointer. Vulkan overrides to bind a descriptor-array element.
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
    // Only backends with hardware image sampling override this; the default
    // rejects a tex.sample() lowered for a backend without it.
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
    // Only backends with an unfiltered image read override this; the default
    // rejects a tex.fetch() lowered for a backend without it.
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
    // Only backends with a hardware storage-image write override this; the
    // default rejects an img.store() lowered for a backend without it.
    throw cajeta::Exception(
        "XPU kernel lowering: storage-image write not supported on backend '" +
        std::string(name()) + "'", "XPU-N01");
}

llvm::Value* LoweringTarget::loadImage(llvm::IRBuilderBase& /*b*/,
                                       llvm::Module& /*m*/,
                                       llvm::Value* /*imgHandle*/,
                                       llvm::Value* /*x*/, llvm::Value* /*y*/) {
    // Only backends with a hardware storage-image read override this; the
    // default rejects an img.load() lowered for a backend without it.
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
    llvm::Value* acc, bool aSigned, bool cSigned) {
    return vecops::idotWiden(b, a, c, acc, aSigned, cSigned);
}

// Default 4-bit table lookup: the portable spill-and-gather (correct on
// CPU/AMD/NVIDIA/Vulkan). AMDGPU overrides this to emit v_perm_b32.
llvm::Value* LoweringTarget::byteLut16(
    llvm::IRBuilderBase& b, llvm::Module& /*m*/, llvm::Value* indices,
    llvm::Value* table) {
    return vecops::lut4Portable(b, indices, table);
}

// Map a user MemoryOrder to an LLVM AtomicOrdering; Default falls back to the
// backend's established default ordering.
llvm::AtomicOrdering LoweringTarget::toAtomicOrdering(
        MemoryOrder o, llvm::AtomicOrdering fallback) {
    switch (o) {
        case MemoryOrder::Relaxed: return llvm::AtomicOrdering::Monotonic;
        case MemoryOrder::Acquire: return llvm::AtomicOrdering::Acquire;
        case MemoryOrder::Release: return llvm::AtomicOrdering::Release;
        case MemoryOrder::AcqRel:  return llvm::AtomicOrdering::AcquireRelease;
        case MemoryOrder::SeqCst:  return llvm::AtomicOrdering::SequentiallyConsistent;
        case MemoryOrder::Default:
        default:                   return fallback;
    }
}

// cmpxchg failure ordering: LLVM requires it be no stronger than success and
// never Release/AcquireRelease. Downgrade those; pass the rest through.
llvm::AtomicOrdering LoweringTarget::casFailureOrdering(
        llvm::AtomicOrdering success) {
    switch (success) {
        case llvm::AtomicOrdering::Release:        return llvm::AtomicOrdering::Monotonic;
        case llvm::AtomicOrdering::AcquireRelease: return llvm::AtomicOrdering::Acquire;
        default:                                   return success;
    }
}

// Default scoped memory fence: a system-scope `fence` at `order`, with
// Default/Relaxed promoted to AcquireRelease since a relaxed fence is a no-op.
// GPU backends override.
void LoweringTarget::memoryFence(llvm::IRBuilderBase& b, llvm::Module& /*m*/,
                                 FenceScope /*scope*/, MemoryOrder order) {
    llvm::AtomicOrdering ord =
        toAtomicOrdering(order, llvm::AtomicOrdering::AcquireRelease);
    if (ord == llvm::AtomicOrdering::Monotonic)      // a relaxed fence is a no-op
        ord = llvm::AtomicOrdering::AcquireRelease;
    b.CreateFence(ord);
}

// Default async global-to-shared copy: a SYNCHRONOUS thread-strided copy, the
// same structure as CoopStage.panel's contiguous case. Bit-identical to the
// native path, just without compute/transfer overlap. AMDGPU overrides.
void LoweringTarget::asyncCopy(llvm::IRBuilderBase& b, llvm::Module& m,
                               llvm::Value* dstBase, llvm::Type* dstElem,
                               llvm::Value* dstOffset, llvm::Value* srcBase,
                               llvm::Type* srcElem, llvm::Value* srcOffset,
                               llvm::Value* count) {
    llvm::LLVMContext& ctx = m.getContext();
    llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
    llvm::Function* fn = b.GetInsertBlock()->getParent();
    auto i32of = [&](llvm::Value* v) { return b.CreateZExtOrTrunc(v, i32); };
    llvm::Value* tid  = i32of(threadId(b, m, 0));
    llvm::Value* nthr = i32of(workgroupDim(b, m, 0));
    llvm::Value* cnt  = i32of(count);
    llvm::Value* dOff = i32of(dstOffset);
    llvm::Value* sOff = i32of(srcOffset);
    llvm::BasicBlock* pred = b.GetInsertBlock();
    auto* head = llvm::BasicBlock::Create(ctx, "asynccopy.head", fn);
    auto* body = llvm::BasicBlock::Create(ctx, "asynccopy.body", fn);
    auto* exit = llvm::BasicBlock::Create(ctx, "asynccopy.exit", fn);
    b.CreateBr(head);
    b.SetInsertPoint(head);
    llvm::PHINode* e = b.CreatePHI(i32, 2, "asynccopy.e");
    e->addIncoming(tid, pred);
    b.CreateCondBr(b.CreateICmpULT(e, cnt), body, exit);
    b.SetInsertPoint(body);
    llvm::Value* sPtr = bufferElementPtr(
        b, m, srcBase, srcElem, b.CreateZExt(b.CreateAdd(sOff, e), i64));
    llvm::Value* v = b.CreateLoad(srcElem, sPtr, "asynccopy.v");
    llvm::Value* dPtr = bufferElementPtr(
        b, m, dstBase, dstElem, b.CreateZExt(b.CreateAdd(dOff, e), i64));
    b.CreateStore(v, dPtr);
    llvm::Value* eNext = b.CreateAdd(e, nthr);
    e->addIncoming(eNext, body);
    b.CreateBr(head);
    b.SetInsertPoint(exit);
}

// Default async-copy group commit/wait: no-ops. The synchronous fallback above
// already landed the data, so there is nothing in flight to track or wait on.
void LoweringTarget::asyncCommit(llvm::IRBuilderBase&, llvm::Module&) {}

void LoweringTarget::asyncWait(llvm::IRBuilderBase&, llvm::Module&,
                               llvm::Value*) {}

// Default scheduling hints: no-ops. A scheduling hint never changes a kernel's
// result, so a backend without a native intrinsic simply drops it.
void LoweringTarget::schedBarrier(llvm::IRBuilderBase&, llvm::Module&,
                                  uint32_t) {}
void LoweringTarget::schedGroupBarrier(llvm::IRBuilderBase&, llvm::Module&,
                                       uint32_t, uint32_t, uint32_t) {}
void LoweringTarget::schedPriority(llvm::IRBuilderBase&, llvm::Module&,
                                   uint32_t) {}
void LoweringTarget::schedPipelineOpt(llvm::IRBuilderBase&, llvm::Module&,
                                      uint32_t) {}

// Default LDS swizzle: the identity (no permutation). Swizzling is a perf-only
// transform; a backend without it stays correct by addressing the tile linearly.
llvm::Value* LoweringTarget::swizzleAddr(llvm::IRBuilderBase&, llvm::Value* idx,
                                         uint32_t /*stride*/) {
    return idx;
}

// Default block padding: the identity. Like swizzling, it is a perf-only layout
// transform; a backend without it stays correct by addressing the tile linearly.
llvm::Value* LoweringTarget::blockPadAddr(llvm::IRBuilderBase&, llvm::Value* idx,
                                          uint32_t /*period*/, uint32_t /*pad*/) {
    return idx;
}

// Default device printf: rejected. CPU + NVPTX override; AMD/Vulkan need
// hostcall / DebugPrintf runtime integration (deferred).
void LoweringTarget::devicePrintf(llvm::IRBuilderBase&, llvm::Module&,
                                  llvm::Value*, llvm::ArrayRef<llvm::Value*>) {
    throw cajeta::Exception(
        "Debug.printf is not supported on this backend (CPU + NVPTX only; AMD "
        "hostcall / Vulkan DebugPrintf deferred)", "XPU-N01");
}

// Default specialization-constant read for the device-baking backends: the
// runtime sets `__cajeta_xpu_spec_count` + `__cajeta_xpu_spec_values[]` per
// launch, and zero-initialized globals fall back to the baked default.
namespace {
constexpr unsigned kSpecConstantAS = 4;     // NVPTX/AMDGCN constant memory
constexpr unsigned kSpecMaxUserSlots = 60;  // matches the runtime cap (4 + 60 <= 64)

// Get-or-create the two constant-memory spec globals (count + raw 4-byte words).
// Fixed names so the runtime can resolve them; external linkage + zeroinit so
// they are a visible definition the host can write.
std::pair<llvm::GlobalVariable*, llvm::GlobalVariable*>
specConstantGlobals(llvm::Module& m) {
    llvm::LLVMContext& ctx = m.getContext();
    auto* i32 = llvm::Type::getInt32Ty(ctx);
    auto* arrTy = llvm::ArrayType::get(i32, kSpecMaxUserSlots);
    auto* countG = m.getGlobalVariable("__cajeta_xpu_spec_count", true);
    if (!countG) {
        countG = new llvm::GlobalVariable(
            m, i32, /*isConstant=*/false, llvm::GlobalValue::ExternalLinkage,
            llvm::ConstantInt::get(i32, 0), "__cajeta_xpu_spec_count", nullptr,
            llvm::GlobalValue::NotThreadLocal, kSpecConstantAS);
    }
    auto* valsG = m.getGlobalVariable("__cajeta_xpu_spec_values", true);
    if (!valsG) {
        valsG = new llvm::GlobalVariable(
            m, arrTy, /*isConstant=*/false, llvm::GlobalValue::ExternalLinkage,
            llvm::ConstantAggregateZero::get(arrTy), "__cajeta_xpu_spec_values",
            nullptr, llvm::GlobalValue::NotThreadLocal, kSpecConstantAS);
    }
    return {countG, valsG};
}

// Emit `(slot u< count) ? values[slot] : <default-as-i32-word>`, returning the
// raw i32 word (the caller bitcasts to float for getf).
llvm::Value* emitSpecConstantWordRead(llvm::IRBuilderBase& b, llvm::Module& m,
                                      unsigned slot, llvm::Value* defaultWord) {
    if (slot >= kSpecMaxUserSlots) return defaultWord;   // out of range Ã¢ÂÂ default
    llvm::LLVMContext& ctx = m.getContext();
    auto* i32 = llvm::Type::getInt32Ty(ctx);
    auto [countG, valsG] = specConstantGlobals(m);
    llvm::Value* count = b.CreateLoad(i32, countG, /*isVolatile=*/true, "spec.count");
    llvm::Value* inRange = b.CreateICmpULT(
        llvm::ConstantInt::get(i32, slot), count, "spec.inrange");
    llvm::Value* slotPtr = b.CreateInBoundsGEP(
        valsG->getValueType(), valsG,
        {llvm::ConstantInt::get(i32, 0), llvm::ConstantInt::get(i32, slot)},
        "spec.slot");
    llvm::Value* word = b.CreateLoad(i32, slotPtr, /*isVolatile=*/true, "spec.word");
    return b.CreateSelect(inRange, word, defaultWord, "spec.sel");
}
}  // namespace

llvm::Value* LoweringTarget::specConstantI32(llvm::IRBuilderBase& b,
                                             llvm::Module& m, unsigned slot,
                                             int32_t defaultValue) {
    llvm::Value* def = llvm::ConstantInt::get(
        llvm::Type::getInt32Ty(m.getContext()), (uint64_t) (int64_t) defaultValue,
        /*isSigned=*/true);
    return emitSpecConstantWordRead(b, m, slot, def);
}

// f32 companion Ã¢ÂÂ same constant-memory read, on the raw word: default packed as
// its bit pattern, the selected word bitcast back to float.
llvm::Value* LoweringTarget::specConstantF32(llvm::IRBuilderBase& b,
                                             llvm::Module& m, unsigned slot,
                                             float defaultValue) {
    llvm::LLVMContext& ctx = m.getContext();
    auto* i32 = llvm::Type::getInt32Ty(ctx);
    auto* f32 = llvm::Type::getFloatTy(ctx);
    llvm::Value* defWord = b.CreateBitCast(
        llvm::ConstantFP::get(f32, (double) defaultValue), i32, "spec.defbits");
    llvm::Value* word = emitSpecConstantWordRead(b, m, slot, defWord);
    return b.CreateBitCast(word, f32, "spec.f32");
}

// Default float atomic: a system-scope atomicrmw at `order` (Default Ã¢ÂÂ relaxed/
// Monotonic). Selects the native global FP atomic on AMDGPU/NVPTX and a
// lock/cmpxchg on CPU. Vulkan overrides (Device scope + AcquireRelease default).
llvm::Value* LoweringTarget::atomicFloatRMW(
    llvm::IRBuilderBase& b, llvm::Module& /*m*/, AtomicFloatOp op,
    llvm::Value* ptr, llvm::Value* value, MemoryOrder order) {
    llvm::AtomicRMWInst::BinOp binop =
        op == AtomicFloatOp::Add ? llvm::AtomicRMWInst::FAdd
      : op == AtomicFloatOp::Min ? llvm::AtomicRMWInst::FMin
                                 : llvm::AtomicRMWInst::FMax;
    return b.CreateAtomicRMW(binop, ptr, value, llvm::MaybeAlign(),
                             toAtomicOrdering(order,
                                              llvm::AtomicOrdering::Monotonic));
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
    llvm::Value* ptr, llvm::Value* value, bool isSigned, MemoryOrder order) {
    return b.CreateAtomicRMW(atomicIntBinOp(op, isSigned), ptr, value,
                             llvm::MaybeAlign(),
                             toAtomicOrdering(order,
                                              llvm::AtomicOrdering::Monotonic));
}

// Default compare-exchange: a cmpxchg at `order` (Default Ã¢ÂÂ relaxed); returns the
// OLD value (element 0 of the {value, success} aggregate). Vulkan overrides scope.
llvm::Value* LoweringTarget::atomicCompareExchange(
    llvm::IRBuilderBase& b, llvm::Module& /*m*/, llvm::Value* ptr,
    llvm::Value* expected, llvm::Value* desired, MemoryOrder order) {
    llvm::AtomicOrdering success =
        toAtomicOrdering(order, llvm::AtomicOrdering::Monotonic);
    llvm::Value* pair = b.CreateAtomicCmpXchg(
        ptr, expected, desired, llvm::MaybeAlign(),
        success, casFailureOrdering(success));
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

// Ray query (SPV_KHR_ray_query) is Vulkan-only Ã¢ÂÂ only SpirvTarget overrides
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
    uint32_t /*rows*/, uint32_t /*cols*/, uint32_t /*use*/,
    uint32_t /*swizzleStride*/, LdsBlockPad /*blockPad*/) {
    throw coopMatrixUnsupported(name());
}

void LoweringTarget::coopMatrixStore(
    llvm::IRBuilderBase& /*b*/, llvm::Module& /*m*/, llvm::Value* /*ptr*/,
    llvm::Value* /*matrixVal*/, llvm::Value* /*layout*/, llvm::Value* /*stride*/,
    uint32_t /*rows*/, uint32_t /*cols*/, uint32_t /*use*/,
    uint32_t /*swizzleStride*/, LdsBlockPad /*blockPad*/) {
    throw coopMatrixUnsupported(name());
}

llvm::Value* LoweringTarget::coopMatrixMulAdd(
    llvm::IRBuilderBase& /*b*/, llvm::Module& /*m*/, llvm::Value* /*a*/,
    llvm::Value* /*bMat*/, llvm::Value* /*c*/, llvm::Type* /*matrixType*/,
    uint32_t /*signFlags*/) {
    throw coopMatrixUnsupported(name());
}

llvm::Value* LoweringTarget::coopMatrixSplat(
    llvm::IRBuilderBase& /*b*/, llvm::Module& /*m*/, llvm::Value* /*value*/,
    llvm::Type* /*matrixType*/) {
    throw coopMatrixUnsupported(name());
}

llvm::Value* LoweringTarget::coopMatrixEpilogueAccum(
    llvm::IRBuilderBase& /*b*/, llvm::Module& /*m*/, llvm::Value* /*accVal*/,
    llvm::Value* /*faccVal*/, llvm::Value* /*rowFPtr*/, llvm::Type* /*rowETy*/,
    llvm::Value* /*colFPtr*/, llvm::Type* /*colETy*/,
    llvm::Value* /*rowGPtr*/, llvm::Value* /*colGPtr*/,
    llvm::Value* /*colFScalar*/, llvm::Value* /*colGScalar*/) {
    // Unreachable by construction: the tier scan demotes verb-using kernels on
    // any backend where coopMatrixEpilogueSupported() is false.
    throw coopMatrixUnsupported(name());
}

llvm::Type* LoweringTarget::bufferParamType(llvm::Module& m,
                                            llvm::Type* /*elemTy*/) {
    // NVPTX/AMDGPU: a buffer base is a global (addrspace 1) pointer Ã¢ÂÂ the same
    // type createKernel gives a kernel buffer param, so a helper arg matches it.
    return llvm::PointerType::get(m.getContext(), kGlobalAS);
}

llvm::Type* LoweringTarget::textureParamType(llvm::Module& m) {
    // Default (NVPTX emit-only): cudaTextureObject_t is a 64-bit handle by value.
    // AMDGPU overrides to ptr addrspace(4) (the HIP texture object).
    return llvm::Type::getInt64Ty(m.getContext());
}

// Admit the kernel parameters: Buffer<T>/arrays carry an element type,
// primitives a scalar type. The classification is backend-neutral; HOW they
// become a signature is the backend's call.
static std::vector<LoweringTarget::KernelParam> collectParams(
        const MethodPtr& method, llvm::LLVMContext& ctx) {
    std::vector<LoweringTarget::KernelParam> params;
    for (auto& p : method->getParameterList()) {
        if (!p) continue;
        if (p->getName() == "this") continue;
        CajetaTypePtr t = p->getType();
        if (isTextureType(t) || isTexture3DType(t) || isTexture1DType(t) ||
            isTexture2DArrayType(t) || isTextureCubeType(t)) {
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
            params.push_back({p->getName(), /*isBuffer=*/false,
                              llvm::Type::getFloatTy(ctx), /*isSigned=*/true,
                              /*isTexture=*/false, /*isSampler=*/false,
                              /*isAccelStruct=*/false, /*isImage=*/true});
        } else if (isSamplerType(t)) {
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
            params.push_back({p->getName(), /*isBuffer=*/false,
                              llvm::Type::getInt64Ty(ctx), /*isSigned=*/false,
                              /*isTexture=*/false, /*isSampler=*/false,
                              /*isAccelStruct=*/true});
        } else if (auto arr = std::dynamic_pointer_cast<CajetaArray>(t);
                   arr && isBufferType(arr->getElementType())) {
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
            params.push_back({p->getName(), /*isBuffer=*/false, mt,
                              /*isSigned=*/false});
        } else if (llvm::Type* vt = deviceVectorType(t, ctx)) {
            params.push_back({p->getName(), /*isBuffer=*/false, vt,
                              /*isSigned=*/false});
        } else if (DeviceStructInfo si = deviceStructInfo(t, ctx); si.type) {
            params.push_back({p->getName(), /*isBuffer=*/false, si.type,
                              /*isSigned=*/false});
        } else {
            unsupported("kernel parameter type '" +
                        (t ? t->toCanonical() : std::string("?")) + "'");
        }
        if (!params.empty() &&
            p->findAnnotation(XpuAttr::PushConstant) != nullptr) {
            LoweringTarget::KernelParam& last = params.back();
            if (!last.isBuffer && !last.isTexture && !last.isImage &&
                !last.isSampler && !last.isAccelStruct && !last.isBufferArray) {
                last.isPushConstant = true;
            }
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
            kind = KernelParamInfo::AccelStruct;
        } else if (p.isBufferArray) {
            kind = KernelParamInfo::BufferArray;   // checked before isBuffer (both true)
        } else if (p.isBuffer) {
            kind = KernelParamInfo::Buffer;
        } else if (p.type) {
            // The marshalled by-value footprint must use the HOST module's real
            // DataLayout and, for an aggregate or vector, its alloc size:
            // getScalarSizeInBits() on a vector returns the ELEMENT width.
            bytes = (p.type->isStructTy() || p.type->isVectorTy())
                ? (unsigned) dl.getTypeAllocSize(p.type)
                : (p.type->getScalarSizeInBits() + 7u) / 8u;
        }
        info.push_back({kind, bytes});
    }
    return info;
}

// Base default for wave rotate: the width-agnostic shuffle form, so NVPTX/AMD/
// CPU get rotate for free and Vulkan overrides with the native op. Keep the
// (laneId+delta) mod width semantics in lock-step with that native op.
llvm::Value* LoweringTarget::waveRotate(llvm::IRBuilderBase& b, llvm::Module& m,
                                        llvm::Value* value, llvm::Value* delta) {
    llvm::Value* lane = waveLaneId(b, m);
    llvm::Value* width = waveWidth(b, m);
    llvm::Value* src = b.CreateURem(
        b.CreateAdd(lane, delta), width, "wave.rotate.src");
    return waveShuffle(b, m, value, src);
}

// Base default for the exclusive prefix scan: a width-agnostic Hillis-Steele
// scan over the existing wave seams, so a backend with no native op still gets
// one. Vulkan overrides to OpGroupNonUniform ExclusiveScan; CPU to a VFABI form.
llvm::Value* LoweringTarget::waveScan(llvm::IRBuilderBase& b, llvm::Module& m,
                                      WaveScanOp op, llvm::Value* value) {
    llvm::LLVMContext& ctx = m.getContext();
    llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Value* lane = waveLaneId(b, m);
    llvm::Value* width = waveWidth(b, m);
    auto combine = [&](llvm::Value* x, llvm::Value* y) {
        return op == WaveScanOp::Sum ? b.CreateAdd(x, y) : b.CreateMul(x, y);
    };
    // Inclusive Hillis-Steele: acc[i] op= acc[i-d] for d = 1,2,4,... < width, so
    // the trip count is log2(width).
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
    // pred = lane >= d; read acc from lane-d, clamped to a valid lane when pred
    // is false since the select discards it.
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

// The shared XOR butterfly: every lane combines with its partner at distance d
// for d = 1, 2, 4, ... while d < `bound`. bound == waveWidth gives a whole-wave
// reduce; bound == seg gives an independent reduce per aligned seg-lane group.
static llvm::Value* waveReduceF32Butterfly(LoweringTarget& t,
                                           llvm::IRBuilderBase& b,
                                           llvm::Module& m,
                                           LoweringTarget::WaveReduceFOp op,
                                           llvm::Value* value,
                                           llvm::Value* bound) {
    using WaveReduceFOp = LoweringTarget::WaveReduceFOp;
    llvm::LLVMContext& ctx = m.getContext();
    llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
    llvm::Value* lane = t.waveLaneId(b, m);
    llvm::Value* width = bound;
    llvm::Function* fn = b.GetInsertBlock()->getParent();
    llvm::BasicBlock* preheader = b.GetInsertBlock();
    llvm::BasicBlock* loop = llvm::BasicBlock::Create(ctx, "fred.loop", fn);
    llvm::BasicBlock* done = llvm::BasicBlock::Create(ctx, "fred.done", fn);
    b.CreateBr(loop);
    b.SetInsertPoint(loop);
    llvm::PHINode* accPhi = b.CreatePHI(f32, 2, "fred.acc");
    llvm::PHINode* dPhi = b.CreatePHI(i32, 2, "fred.d");
    accPhi->addIncoming(value, preheader);
    dPhi->addIncoming(llvm::ConstantInt::get(i32, 1), preheader);
    llvm::Value* src = b.CreateXor(lane, dPhi, "fred.src");
    llvm::Value* otherBits = t.waveShuffleDivergent(
        b, m, b.CreateBitCast(accPhi, i32), src);
    llvm::Value* other = b.CreateBitCast(otherBits, f32);
    llvm::Value* acc = op == WaveReduceFOp::Sum
        ? b.CreateFAdd(accPhi, other, "fred.sum")
        : b.CreateBinaryIntrinsic(llvm::Intrinsic::maxnum, accPhi, other,
                                  llvm::FMFSource(), "fred.max");
    llvm::Value* dNext = b.CreateShl(dPhi, 1);
    accPhi->addIncoming(acc, loop);
    dPhi->addIncoming(dNext, loop);
    b.CreateCondBr(b.CreateICmpULT(dNext, width), loop, done);
    b.SetInsertPoint(done);
    llvm::PHINode* res = b.CreatePHI(f32, 1, "fred.result");
    res->addIncoming(acc, loop);
    return res;
}

llvm::Value* LoweringTarget::waveReduceF32(llvm::IRBuilderBase& b,
                                           llvm::Module& m, WaveReduceFOp op,
                                           llvm::Value* value) {
    return waveReduceF32Butterfly(*this, b, m, op, value, waveWidth(b, m));
}

llvm::Value* LoweringTarget::waveReduceF32Segmented(llvm::IRBuilderBase& b,
                                                    llvm::Module& m,
                                                    WaveReduceFOp op,
                                                    llvm::Value* value,
                                                    llvm::Value* seg) {
    // A seg wider than the wave is not this primitive's job - that regime
    // combines across waves through LDS - so clamp to min(seg, width).
    llvm::Value* width = waveWidth(b, m);
    llvm::Value* useSeg = b.CreateSelect(
        b.CreateICmpULT(seg, width), seg, width, "seg.clamp");
    return waveReduceF32Butterfly(*this, b, m, op, value, useSeg);
}

// Quad (2x2) op defaults: width-agnostic forms built on the wave seams, so
// NVPTX/AMDGPU/CPU take these and Vulkan overrides with native ops. A quad is
// the four lanes [laneId & ~3 .. +3].
llvm::Value* LoweringTarget::quadBroadcast(llvm::IRBuilderBase& b,
                                           llvm::Module& m, llvm::Value* value,
                                           llvm::Value* index) {
    // Read from lane (laneId & ~3) + index Ã¢ÂÂ the `index`-th lane of this quad.
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

    // An explicit @Occupancy override is applied before the auto budgeting, so
    // it wins.
    if (auto attr = XpuKernelAttr::from(*method); attr && attr->hasOccupancy())
        target.applyOccupancy(fn, *attr);

    DeviceLowerer lowerer(deviceModule, fn, target);
    lowerer.setParams(std::move(params));
    // Per-kernel cache of lowered @Device helpers, so each is emitted once and
    // recursion is caught; the functions themselves live in the module.
    DeviceLowerer::DeviceFnCache deviceFns;
    lowerer.setDeviceContext(method->getParent(), &deviceFns);
    lowerer.lowerBody(method);
    // If the kernel used a cross-lane subgroup op, ask the backend for maximal
    // reconvergence (a no-op where it is not modelled).
    if (lowerer.usedSubgroupOp())
        target.onSubgroupOpsUsed(fn, deviceModule);
    // Honour the author's parameter declarations against the LOWERED body:
    // `@Streaming` tags loads/stores non-temporal where the backend supports it,
    // `@Access(m)` is checked for a contradiction (a compile error).
    applyAccessDeclarations(*fn, method, target.supportsNontemporal());
    return fn;
}

} // namespace xpu
} // namespace cajeta
