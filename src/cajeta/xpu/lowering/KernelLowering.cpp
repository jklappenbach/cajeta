//
// Shared @Kernel AST → device llvm::Function lowering — see header.
//

#include "KernelLowering.h"
#include "LoweringTarget.h"

#include "../../method/Method.h"
#include "../../type/FormalParameter.h"
#include "../../type/CajetaType.h"
#include "../../type/CajetaClass.h"
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

#include <map>
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
    static const std::string kPrefix = "cajeta.xpu.core.Buffer";
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

    void lowerBody(const MethodPtr& method) {
        if (!cls) cls = method->getParent();   // for @Device helper resolution
        builder.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", fn));
        // Materialize params now that the entry block exists. Scalars get a
        // mutable alloca slot (so they can be reassigned / serve as loop
        // counters); buffers keep their base/handle. HOW a param's runtime
        // value is obtained is the backend's call (target.materializeParam):
        // NVPTX/AMDGPU read fn->getArg(idx); Vulkan binds a descriptor here.
        unsigned idx = 0;
        for (auto& p : kparams) {
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
                // value (a ptr on CPU); read only by `tex.sample(...)`.
                textureHandles[p.name] = v;
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
    // Texture2D / Sampler kernel params (Item 8): the materialized backend
    // handle per name. A `tex.sample(s, u, v)` looks the texture up here and the
    // sampler arg up in samplerHandles, then hands both to target.sampleTexture.
    std::map<std::string, llvm::Value*> textureHandles;  // texture name -> handle
    std::map<std::string, llvm::Value*> samplerHandles;  // sampler name -> descriptor
    // AccelerationStructure kernel params and RayQuery body locals (cajeta-gpu
    // Part C ray query). An AS param's materialized descriptor handle is kept by
    // name; a RayQuery local's opaque alloca (the OpVariable Function) by name.
    // rq.initialize(as, ...) looks the AS up in accelHandles; every RayQuery op
    // takes the alloca from rayQuerySlots.
    std::map<std::string, llvm::Value*> accelHandles;    // AS name -> descriptor
    std::map<std::string, llvm::Value*> rayQuerySlots;   // RayQuery name -> alloca
    // CooperativeMatrix locals (CM4): the alloca slot holds the opaque tile
    // value (an OpTypeCooperativeMatrixKHR loaded/stored as a whole object);
    // matrixType is that device type, retained so ops can load/store the slot.
    struct CoopMatrixSlot { llvm::Value* alloca; llvm::Type* matrixType; };
    std::map<std::string, CoopMatrixSlot> coopMatrixSlots;

    std::vector<LoweringTarget::KernelParam> kparams;  // admitted params
    bool paramsAsArgs = false;  // true for @Device helpers (params are fn args)
    std::map<std::string, bool> bufferElemSigned;  // buffer name -> elem signed?
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
                rayQuerySlots[nm] = entryAlloca(target.rayQueryType(mod), nm);
                continue;
            }
            // CooperativeMatrix local (CM4): a device-only subgroup tile. The
            // alloca holds the opaque OpTypeCooperativeMatrixKHR value; load/
            // splat/mma write it, store/mma read it. Backend-gated: coopMatrixType
            // throws on a non-Vulkan backend (XPU-N03). A `stack CooperativeMatrix
            // <...>()` initializer is just the construction — no value to store.
            if (isCooperativeMatrixType(declType)) {
                llvm::Type* matTy = buildCoopMatrixType(declType);
                coopMatrixSlots[nm] = { entryAlloca(matTy, nm), matTy };
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
            if (args.size() != 1) unsupported("Vector.dot expects one argument");
            llvm::Value* other = lowerExpr(args[0].expression);
            return vecops::dot(builder, self, other, isFloat);
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
        unsupported("unknown Quaternion method '" + name + "' (slerp is a "
                    "follow-on; use nlerp)");
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
            if (name == "shuffleSync") {
                if (args.size() != 2) unsupported("Wave.shuffleSync arity");
                llvm::Value* value = lowerExpr(args[0].expression);
                llvm::Value* srcLane = lowerExpr(args[1].expression);
                return target.waveShuffle(builder, mod, value, srcLane);
            }
            if (name == "ballotSync") {
                if (args.size() != 1) unsupported("Wave.ballotSync arity");
                llvm::Value* pred = toI1(lowerExpr(args[0].expression));
                return target.waveBallot(builder, mod, pred);
            }
            if (name == "reduceSum") {
                if (args.size() != 1) unsupported("Wave.reduceSum arity");
                return target.waveReduceSum(builder, mod,
                                            lowerExpr(args[0].expression));
            }
        } else if (recv == "Math") {
            // Math.<fn>(...) inside a kernel — fully handled (returns a value or
            // a clean diagnostic). Mirrors the host Math lowering.
            return lowerMathCall(name, mc);
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
        // Texture2D.sample(sampler, u, v) (Item 8). `recv` names a texture kernel
        // param; the first arg is a sampler kernel param, then the normalized
        // (u, v) coords. Lowered to the backend image-sample seam.
        if (name == "sample") {
            auto th = textureHandles.find(recv);
            if (th != textureHandles.end()) {
                const auto& args = mc->getParameters();
                if (args.size() != 3)
                    unsupported("Texture2D.sample expects (Sampler, u, v)");
                llvm::Value* samp = resolveSamplerArg(args[0].expression);
                llvm::Value* u = toFloat(lowerExpr(args[1].expression));
                llvm::Value* v = toFloat(lowerExpr(args[2].expression));
                return target.sampleTexture(builder, mod, th->second, samp, u, v);
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
        unsupported("Texture2D.sample: first argument must be a Sampler "
                    "kernel parameter");
    }

    // RayQuery op dispatch (Part C). `rqPtr` is the RayQuery alloca; the call is
    // lowered to the backend ray-query seam (Vulkan llvm.spv.ray.query.*).
    llvm::Value* lowerRayQueryMethod(
            llvm::Value* rqPtr, const std::string& name,
            const std::shared_ptr<MethodCallExpression>& mc) {
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
        if (name == "candidatePrimitiveIndex") {
            if (!args.empty())
                unsupported("RayQuery." + name + " takes no arguments");
            // Candidate intersection (selector 0) primitive index.
            return target.rayQueryIntersectionPrimitiveIndex(
                builder, mod, rqPtr, llvm::ConstantInt::get(i32, 0));
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
    llvm::Type* buildCoopMatrixType(const CajetaTypePtr& declType) {
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
        return target.coopMatrixType(mod, elem, (uint32_t) rows->getValue(),
                                     (uint32_t) cols->getValue(),
                                     (uint32_t) use->getValue());
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
                llvm::Value* v = target.coopMatrixLoad(builder, mod, ptr, layout,
                                                       stride, slot.matrixType);
                builder.CreateStore(v, slot.alloca);
            } else {
                llvm::Value* v = builder.CreateLoad(slot.matrixType, slot.alloca,
                                                    recv + ".val");
                target.coopMatrixStore(builder, mod, ptr, v, layout, stride);
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
        auto asFp = [&](llvm::Value* v) -> llvm::Value* {
            if (v->getType()->isFloatingPointTy()) return v;
            return builder.CreateSIToFP(v, f32);          // int -> f32
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
        unsupported("Math." + name + " is not yet available in a kernel on "
                    "device — transcendentals (sin/cos/tan/exp/log/pow) need "
                    "device-library linking (pending); the natively-lowering "
                    "ops are sqrt/floor/ceil/trunc/round/abs/min/max/fma");
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
        auto* fnTy = llvm::FunctionType::get(retTy, tys, /*vararg=*/false);
        std::string fname = "__cajeta_xpu_dev." +
            (cls ? cls->toCanonical() + "." : std::string()) + m->getName();
        auto* hfn = llvm::Function::Create(
            fnTy, llvm::GlobalValue::InternalLinkage, fname, &mod);
        hfn->addFnAttr(llvm::Attribute::AlwaysInline);
        unsigned i = 0;
        for (auto& p : params) hfn->getArg(i++)->setName(p.name);

        DeviceLowerer sub(mod, hfn, target);
        sub.setParams(params);
        sub.setParamsAsArgs(true);   // helper params are plain fn args, not
                                     // kernel descriptors/SSBOs (Vulkan)
        sub.setDeviceContext(cls, deviceFns);
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
    llvm::Value* applyBinOp(BinaryOp op, llvm::Value* l, llvm::Value* r,
                            bool sign, bool /*fpHint*/) {
        unifyOperands(l, r, sign);
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
        else if (p.isTexture)
            tys.push_back(textureParamType(m));   // texture handle (ptr/i64)
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

llvm::Value* LoweringTarget::sampleTexture(llvm::IRBuilderBase& /*b*/,
                                           llvm::Module& /*m*/,
                                           llvm::Value* /*texHandle*/,
                                           llvm::Value* /*samplerHandle*/,
                                           llvm::Value* /*u*/,
                                           llvm::Value* /*v*/) {
    // Only backends with hardware image sampling override this (CPU emulation,
    // Vulkan OpImageSampleExplicitLod, AMD image_sample). The default rejects a
    // tex.sample() in a kernel lowered for a backend that has not implemented it.
    throw cajeta::Exception(
        "XPU kernel lowering: texture sampling not supported on backend '" +
        std::string(name()) + "'", "XPU-N01");
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
    llvm::Value* /*layout*/, llvm::Value* /*stride*/, llvm::Type* /*matrixType*/) {
    throw coopMatrixUnsupported(name());
}

void LoweringTarget::coopMatrixStore(
    llvm::IRBuilderBase& /*b*/, llvm::Module& /*m*/, llvm::Value* /*ptr*/,
    llvm::Value* /*matrixVal*/, llvm::Value* /*layout*/, llvm::Value* /*stride*/) {
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
        if (isTextureType(t)) {
            // Texture2D (Item 8): a sampled-image handle. `type` is the texel
            // scalar (f32); the backend carries the handle itself (a ptr on CPU,
            // a descriptor on Vulkan). isTexture routes createKernel/
            // materializeParam and the `.sample()` lowering.
            params.push_back({p->getName(), /*isBuffer=*/false,
                              llvm::Type::getFloatTy(ctx), /*isSigned=*/true,
                              /*isTexture=*/true, /*isSampler=*/false});
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
        } else if (p.isSampler) {
            kind = KernelParamInfo::Sampler;
        } else if (p.isAccelStruct) {
            kind = KernelParamInfo::AccelStruct;
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

llvm::Function* lowerKernel(const MethodPtr& method, llvm::Module& deviceModule,
                            LoweringTarget& target) {
    if (!method) unsupported("null kernel method");
    llvm::LLVMContext& ctx = deviceModule.getContext();

    std::vector<LoweringTarget::KernelParam> params = collectParams(method, ctx);

    llvm::Function* fn =
        target.createKernel(deviceModule, method->getName(), params);

    DeviceLowerer lowerer(deviceModule, fn, target);
    lowerer.setParams(std::move(params));
    // A per-kernel cache of lowered @Device helper functions (shared with any
    // nested helper lowering), so each helper is emitted once and recursion is
    // caught. The functions live in the module; the cache is just for dedup.
    DeviceLowerer::DeviceFnCache deviceFns;
    lowerer.setDeviceContext(method->getParent(), &deviceFns);
    lowerer.lowerBody(method);
    return fn;
}

} // namespace xpu
} // namespace cajeta
