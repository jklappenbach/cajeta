//
// Postfix call applied to the result of an expression.
//

#include "CallExpression.h"
#include "Identifier.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaArray.h"
#include "cajeta/type/CajetaFunctionType.h"
#include "cajeta/type/StructureProperty.h"
#include "cajeta/type/Scope.h"
#include "cajeta/error/Exception.h"
#include "cajeta/xpu/core/KernelArgTrait.h"

#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

namespace cajeta {

    CallExpression::CallExpression(
        CajetaParser::ExpressionContext* ctx,
        antlr4::Token* token) : Expression(token) {
        // Mirror MethodCallExpression's parameterList handling: each entry is
        // an optional label (kept with its trailing ':') plus an expression.
        if (auto* paramList = ctx->parameterList()) {
            for (auto& ctxParameterEntry : paramList->parameterEntry()) {
                MethodCallParameter entry;
                entry.expression = Expression::fromContext(
                    ctxParameterEntry->expression());
                if (ctxParameterEntry->parameterLabel()) {
                    entry.label = ctxParameterEntry->parameterLabel()->getText();
                }
                // Caller-side `#x` transfer (Phase 1 of #68).
                if (ctxParameterEntry->REFERENCE()) {
                    entry.callerTransferred = true;
                }
                args.push_back(entry);
            }
        }
    }

    namespace {
        std::string stripColon(const std::string& label) {
            if (!label.empty() && label.back() == ':')
                return label.substr(0, label.size() - 1);
            return label;
        }
    }

    // Lower the XPU launch form
    //   kernel.launch(stream, grid: [gx], block: [bx], sharedBytes: [n])(args...)
    // to a host-side runtime call:
    //   __cajeta_xpu_launch(i8* kernelName, i32 gridX, i32 blockX,
    //                       i32 sharedBytes, ptr argv)
    // `sharedBytes:` (the dynamic-shared-memory byte count, named to match
    // cuLaunchKernel's sharedMemBytes and to avoid the `shared` keyword) is
    // optional, default 0. The label can't be `shared:` — `shared` is the
    // placement keyword, so it doesn't lex as a parameterLabel IDENTIFIER.
    // where `argv` is a stack array of pointers to each kernel argument value
    // (CUDA's kernelParams convention). Buffer<T> args contribute their device
    // pointer (the `deviceHandle` field); scalars contribute their value.
    //
    // The stream argument IS plumbed: its handle is loaded below and passed as
    // the trailing i64 to __cajeta_xpu_launch, which routes it to
    // cuLaunchKernel/hipModuleLaunchKernel for per-stream ordering (async copies
    // + Event cross-stream deps ride the same handle). Vulkan/CPU accept the
    // handle but run synchronously today (no overlap); Stream.sync() drains it.
    llvm::Value* CallExpression::generateCode(CajetaModulePtr module) {
        // Indirect call through a function-typed value — `arr[i](args)`, where
        // the callee is any expression (an array element, a field, a chained
        // result) whose resolved type is a CajetaFunctionType. The callee
        // evaluates to a `ptr` to the closure record; dispatch through the
        // shared closure ABI (same path as the bare `op(args)` form). This is
        // what makes an array-of-function-type callable: `((T)->R)[] ops; …
        // ops[i](x)`. See docs/specification/lang/Lambdas.md.
        if (auto calleeExpr = getCallee()) {
            if (!calleeExpr->getResolvedType()) calleeExpr->resolveTypes(module);
            auto fnType = std::dynamic_pointer_cast<CajetaFunctionType>(
                calleeExpr->getResolvedType());
            if (fnType) {
                llvm::Value* closurePtr = calleeExpr->generateCode(module);
                // The element/field load yields the closure-record `ptr`; an
                // l-value slot (ArrayIndex GEP / field GEP) needs the load.
                closurePtr = loadIfLValue(module, closurePtr, calleeExpr);
                return emitClosureCall(module, closurePtr, fnType, args,
                                       resolvedType);
            }
        }

        auto callee = std::dynamic_pointer_cast<MethodCallExpression>(getCallee());
        if (!callee || callee->getMethodCallName() != "launch") {
            throw Exception(
                "general postfix call — only kernel.launch(...)(...) is "
                "supported", "CAJETA_ERROR_NOT_IMPLEMENTED");
        }

        auto* builder = module->getBuilder();
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);

        // Kernel name: the receiver identifier of `<kernel>.launch(...)`.
        std::string kernelName;
        if (!callee->getChildren().empty()) {
            if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(
                    callee->getChildren()[0])) {
                kernelName = id->getTextValue();
            }
        }
        if (kernelName.empty()) {
            throw Exception("launch receiver is not a kernel name", "XPU-N02");
        }

        // Lower one dimension expression (an array element or a bare scalar) to
        // i32.
        auto lowerOne = [&](const ExpressionPtr& e) -> llvm::Value* {
            llvm::Value* v = e->generateCode(module);
            v = loadIfLValue(module, v, e);
            if (v->getType() != i32Ty) {
                v = builder->CreateIntCast(v, i32Ty, /*isSigned=*/false);
            }
            return v;
        };
        // Extract up to 3 dims from a `grid:`/`block:` value — an array literal
        // `[x]`/`[x,y]`/`[x,y,z]` or a bare scalar (= x). Missing dims default to
        // 1, so a 1-D launch still works unchanged. Returns false if no x dim
        // (an empty array) — the caller treats that as a missing grid:/block:.
        auto lowerDims = [&](const ExpressionPtr& dimExpr,
                             llvm::Value* out[3]) -> bool {
            std::vector<ExpressionPtr> elems;
            if (auto arr =
                    std::dynamic_pointer_cast<ArrayLiteralExpression>(dimExpr)) {
                elems = arr->getElements();
            } else {
                elems.push_back(dimExpr);   // bare scalar = x dim
            }
            if (elems.empty()) return false;
            for (unsigned d = 0; d < 3; ++d)
                out[d] = (d < elems.size()) ? lowerOne(elems[d])
                                            : llvm::ConstantInt::get(i32Ty, 1);
            return true;
        };

        llvm::Value* grid[3]  = {nullptr, nullptr, nullptr};
        llvm::Value* block[3] = {nullptr, nullptr, nullptr};
        bool haveGrid = false, haveBlock = false;
        llvm::Value* sharedBytes = nullptr;   // dynamic shared memory; 0 if absent
        ExpressionPtr streamExpr;             // the unlabeled first param: the Stream
        ExpressionPtr specExpr;               // optional `spec:[v0,v1,…]` overrides
        for (auto& p : callee->getParameters()) {
            std::string label = stripColon(p.label);
            if (label == "grid")  haveGrid  = lowerDims(p.expression, grid);
            else if (label == "block") haveBlock = lowerDims(p.expression, block);
            else if (label == "sharedBytes") {
                llvm::Value* sb[3];
                if (lowerDims(p.expression, sb)) sharedBytes = sb[0];
            }
            // `spec:[v0,v1,…]` — host overrides for the kernel's user
            // specialization constants (Spec.geti slot i = entry i). Optional;
            // absent = every slot reads its compile-time default.
            else if (label == "spec") specExpr = p.expression;
            // The unlabeled first param is the stream — its `handle` field is
            // threaded to the runtime so copies + this launch order on it.
            else if (label.empty()) streamExpr = p.expression;
        }
        if (!haveGrid || !haveBlock) {
            throw Exception("launch requires grid: and block: dimensions",
                            "XPU-N02");
        }
        // `shared:` is the dynamic-shared-memory byte count (cuLaunchKernel
        // sharedMemBytes). Optional — kernels using only static shared memory
        // (or none) omit it; default 0.
        if (!sharedBytes) sharedBytes = llvm::ConstantInt::get(i32Ty, 0);

        // Stream handle (i64): the unlabeled launch arg's `handle` field. 0 = the
        // default stream (preserves the original NULL-stream launch). A real
        // stream orders this launch with the async copies queued on it.
        llvm::Value* streamHandle = nullptr;
        if (streamExpr) {
            llvm::Value* sv = streamExpr->generateCode(module);
            sv = loadIfLValue(module, sv, streamExpr);
            if (!streamExpr->getResolvedType()) streamExpr->resolveTypes(module);
            auto sklass = std::dynamic_pointer_cast<CajetaClass>(
                streamExpr->getResolvedType());
            if (sklass && sklass->toCanonical() == "cajeta.xpu.KernelStream") {
                auto& sprops = sklass->getProperties();
                auto it = sprops.find("handle");
                if (it != sprops.end()) {
                    unsigned idx =
                        (unsigned) sklass->getFieldLlvmIndex(it->second);
                    llvm::Value* hPtr = builder->CreateStructGEP(
                        sklass->getLlvmType(), sv, idx, "stream.handle.ptr");
                    streamHandle =
                        builder->CreateLoad(i64Ty, hPtr, "stream.handle");
                }
            }
        }
        if (!streamHandle) streamHandle = llvm::ConstantInt::get(i64Ty, 0);

        // Marshal kernel args into argv = [N x ptr]; each entry points to a
        // stack slot holding that argument's value.
        size_t n = args.size();
        llvm::ArrayType* argvTy = llvm::ArrayType::get(ptrTy, n ? n : 1);
        llvm::Value* argv = builder->CreateAlloca(argvTy, nullptr, "launch.argv");

        for (size_t i = 0; i < n; ++i) {
            ExpressionPtr argExpr = args[i].expression;
            llvm::Value* v = argExpr->generateCode(module);
            v = loadIfLValue(module, v, argExpr);

            // Buffer<T> arg -> pass its device pointer (the deviceHandle field).
            if (!argExpr->getResolvedType()) argExpr->resolveTypes(module);
            auto klass = std::dynamic_pointer_cast<CajetaClass>(
                argExpr->getResolvedType());
            bool isBuffer = klass &&
                klass->toCanonical().rfind("cajeta.xpu.KernelBuffer", 0) == 0;
            // Texture2D (Item 8): marshalled exactly like a Buffer — its
            // deviceHandle (the runtime texture-object pointer / image handle)
            // flows through the kernelParams slot, and the launch borrows it.
            bool isTexture = klass &&
                (klass->toCanonical().rfind("cajeta.gfx.Texture2D", 0) == 0 ||
                 klass->toCanonical().rfind("cajeta.gfx.Texture3D", 0) == 0 ||
                 klass->toCanonical().rfind("cajeta.gfx.Texture1D", 0) == 0 ||
                 klass->toCanonical().rfind("cajeta.gfx.TextureCube", 0) == 0);
            // NB: the Texture2D prefix above already matches Texture2DArray.
            // AccelerationStructure (Part C): a descriptor-bound device BVH. It
            // marshals via the POD-by-value path below (its deviceHandle is the
            // first field), but the launch borrows it just like a Buffer/Texture2D.
            bool isAccel = xpu::isAccelStructType(argExpr->getResolvedType());

            // Buffer<T>[] arg (bindless) — a descriptor ARRAY of buffers. Resolved
            // type is a CajetaArray whose element is a Buffer<T> (so `klass` above
            // is null). Marshalled as [i64 count, i64 h0 … h(count-1)] into one
            // fixed slot; the runtime binds `count` descriptors at this binding.
            auto arrTy = std::dynamic_pointer_cast<CajetaArray>(
                argExpr->getResolvedType());
            auto bufElemKlass = arrTy ? std::dynamic_pointer_cast<CajetaClass>(
                                            arrTy->getElementType())
                                      : nullptr;
            bool isBufferArray = bufElemKlass &&
                bufElemKlass->toCanonical().rfind("cajeta.xpu.KernelBuffer", 0) == 0;

            // Launch borrow scope (CajetaXPU §3.5/§11): a launch borrows each
            // device-resource arg (Buffer / Texture2D / AccelerationStructure)
            // until the next Stream.sync() / Event.waitHost(); record it so a
            // free/reassign/drop-before-sync is caught (XPU-K02). Hoisted out of
            // the kind-specific marshalling below so AS (a POD-marshalled handle)
            // is covered too.
            if (isBuffer || isTexture || isAccel || isBufferArray) {
                if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(argExpr)) {
                    if (auto sc = module->getScopeStack().peek()) {
                        sc->recordLaunchBorrow(id->getTextValue());
                    }
                }
            }

            llvm::Value* slot;
            if (isBufferArray) {
                // Marshal a descriptor array: a fixed [kMaxBindlessBuffers+1 x i64]
                // slot holding [count, h0 … h(count-1)]. `v` is the array header
                // pointer ({ i64 size, [0 x ptr] data }); each element is a
                // Buffer<T> object pointer whose deviceHandle (field idx) is the
                // device handle. A runtime loop copies the handles. v1 caps the
                // count at kMaxBindlessBuffers (the fixed descriptor-array size).
                const unsigned kMaxBindlessBuffers = 16;
                llvm::Type* arrSlotTy =
                    llvm::ArrayType::get(i64Ty, kMaxBindlessBuffers + 1);
                slot = builder->CreateAlloca(arrSlotTy, nullptr, "arg.bufarray");
                llvm::Type* headerTy = arrTy->getLlvmType();
                auto& bprops = bufElemKlass->getProperties();
                auto bit = bprops.find("deviceHandle");
                if (bit == bprops.end()) {
                    throw Exception("Buffer is missing deviceHandle field",
                                    "XPU-N02");
                }
                unsigned dhIdx =
                    (unsigned) bufElemKlass->getFieldLlvmIndex(bit->second);
                llvm::Type* bufTy = bufElemKlass->getLlvmType();
                llvm::Value* zero = llvm::ConstantInt::get(i64Ty, 0);
                // count = header->size
                llvm::Value* sizePtr = builder->CreateStructGEP(
                    headerTy, v, 0, "bufarr.size.ptr");
                llvm::Value* count =
                    builder->CreateLoad(i64Ty, sizePtr, "bufarr.count");
                // slot[0] = count
                builder->CreateStore(count, builder->CreateInBoundsGEP(
                    arrSlotTy, slot, {zero, zero}, "bufarr.count.slot"));
                // for (b = 0; b < count; ++b) slot[1+b] = data[b]->deviceHandle
                llvm::Function* curFn = builder->GetInsertBlock()->getParent();
                llvm::BasicBlock* condBB =
                    llvm::BasicBlock::Create(ctx, "bufarr.cond", curFn);
                llvm::BasicBlock* bodyBB =
                    llvm::BasicBlock::Create(ctx, "bufarr.body", curFn);
                llvm::BasicBlock* endBB =
                    llvm::BasicBlock::Create(ctx, "bufarr.end", curFn);
                llvm::Value* bVar =
                    builder->CreateAlloca(i64Ty, nullptr, "bufarr.b");
                builder->CreateStore(zero, bVar);
                builder->CreateBr(condBB);
                builder->SetInsertPoint(condBB);
                llvm::Value* bCur = builder->CreateLoad(i64Ty, bVar, "bufarr.b.cur");
                builder->CreateCondBr(
                    builder->CreateICmpULT(bCur, count, "bufarr.more"),
                    bodyBB, endBB);
                builder->SetInsertPoint(bodyBB);
                // elemPtr = &data[b] (holds a Buffer* pointer)
                llvm::Value* elemPtr = builder->CreateInBoundsGEP(
                    headerTy, v,
                    {zero, llvm::ConstantInt::get(
                               llvm::Type::getInt32Ty(ctx),
                               cajeta::CajetaArray::DATA_FIELD_INDEX),
                     bCur},
                    "bufarr.elem.ptr");
                llvm::Value* bufObj =
                    builder->CreateLoad(ptrTy, elemPtr, "bufarr.elem");
                llvm::Value* hPtr = builder->CreateStructGEP(
                    bufTy, bufObj, dhIdx, "bufarr.handle.ptr");
                llvm::Value* handle =
                    builder->CreateLoad(i64Ty, hPtr, "bufarr.handle");
                // slot[1 + b] = handle
                llvm::Value* oneB = builder->CreateAdd(
                    bCur, llvm::ConstantInt::get(i64Ty, 1), "bufarr.slotidx");
                builder->CreateStore(handle, builder->CreateInBoundsGEP(
                    arrSlotTy, slot, {zero, oneB}, "bufarr.h.slot"));
                builder->CreateStore(
                    builder->CreateAdd(bCur,
                                       llvm::ConstantInt::get(i64Ty, 1)),
                    bVar);
                builder->CreateBr(condBB);
                builder->SetInsertPoint(endBB);
            } else if (isBuffer || isTexture) {
                auto& props = klass->getProperties();
                auto it = props.find("deviceHandle");
                if (it == props.end()) {
                    throw Exception("Buffer is missing deviceHandle field",
                                    "XPU-N02");
                }
                unsigned idx = (unsigned) klass->getFieldLlvmIndex(it->second);
                llvm::Value* hPtr = builder->CreateStructGEP(
                    klass->getLlvmType(), v, idx, "buf.handle.ptr");
                llvm::Value* handle =
                    builder->CreateLoad(i64Ty, hPtr, "buf.handle");
                slot = builder->CreateAlloca(i64Ty, nullptr, "arg.buf");
                builder->CreateStore(handle, slot);
            } else if (klass && (xpu::isPodStructType(klass) ||
                                 xpu::isSamplerType(argExpr->getResolvedType()))) {
                // POD struct by value (Item 7) — and the Sampler descriptor
                // (Item 8), which is structurally the same {i32 filterMode,
                // i32 addressMode} packing (admitted by name, not as a POD, but
                // marshalled identically on the CPU/SIMT by-value path).
                // Marshal the FIELDS only into a
                // packed, vtable-stripped buffer — the exact shape the device
                // kernel reads (KernelLowering.cpp deviceStructInfo). `v` is a
                // pointer to the host instance { vtable, fields... }; copy each
                // field out by its host LLVM index into declaration-order slots.
                std::vector<llvm::Type*> ftys;
                std::vector<StructurePropertyPtr> fields;
                for (auto& prop : klass->getPropertyList()) {
                    if (!prop || prop->isStatic()) continue;
                    fields.push_back(prop);
                    ftys.push_back(prop->getType()->getLlvmType());
                }
                llvm::StructType* podTy = llvm::StructType::get(ctx, ftys);
                slot = builder->CreateAlloca(podTy, nullptr, "arg.pod");
                for (unsigned di = 0; di < fields.size(); ++di) {
                    unsigned hostIdx =
                        (unsigned) klass->getFieldLlvmIndex(fields[di]);
                    llvm::Value* src = builder->CreateStructGEP(
                        klass->getLlvmType(), v, hostIdx, "pod.src");
                    llvm::Value* fv =
                        builder->CreateLoad(ftys[di], src, "pod.field");
                    llvm::Value* dst =
                        builder->CreateStructGEP(podTy, slot, di, "pod.dst");
                    builder->CreateStore(fv, dst);
                }
            } else {
                slot = builder->CreateAlloca(v->getType(), nullptr, "arg.scalar");
                builder->CreateStore(v, slot);
            }

            llvm::Value* gep = builder->CreateInBoundsGEP(
                argvTy, argv,
                {llvm::ConstantInt::get(i64Ty, 0),
                 llvm::ConstantInt::get(i64Ty, i)}, "argv.slot");
            builder->CreateStore(slot, gep);
        }

        llvm::Value* argvBase = builder->CreateInBoundsGEP(
            argvTy, argv,
            {llvm::ConstantInt::get(i64Ty, 0),
             llvm::ConstantInt::get(i64Ty, 0)}, "argv.base");
        llvm::Value* nameStr =
            builder->CreateGlobalString(kernelName, "xpu.kernel.name");

        // Host spec-constant overrides: `spec:[v0,v1,…]` lowers to a stack
        // `[N x i32]` (entry i overrides slot i); slot-indexed, sparse tail keeps
        // defaults. Absent → no override (specCount 0). When present we target the
        // versioned `__cajeta_xpu_launch_v3` (deviceId -1, no language surface yet);
        // when absent we keep the exact pre-feature `__cajeta_xpu_launch` emit so
        // every existing launch is byte-identical.
        std::vector<llvm::Value*> specVals;
        if (specExpr) {
            std::vector<ExpressionPtr> elems;
            if (auto arr = std::dynamic_pointer_cast<ArrayLiteralExpression>(specExpr))
                elems = arr->getElements();
            else
                elems.push_back(specExpr);   // bare scalar = slot 0
            for (auto& e : elems) {
                llvm::Value* v = e->generateCode(module);
                v = loadIfLValue(module, v, e);
                llvm::Type* vt = v->getType();
                if (vt->isFloatingPointTy()) {
                    // f32 spec override (Spec.getf): transport the raw 32-bit
                    // pattern, NOT a numeric conversion (else 1.5f → 1). Narrow
                    // an f64 literal to f32 first (spec constants are 32-bit),
                    // then bitcast to the i32 transport word; the consumer
                    // (CPU __cajeta_xpu_cpu_spec_f32 / a Vulkan float
                    // OpSpecConstant) reinterprets it back to float.
                    if (!vt->isFloatTy())
                        v = builder->CreateFPCast(
                            v, llvm::Type::getFloatTy(ctx), "spec.f2f32");
                    v = builder->CreateBitCast(v, i32Ty, "spec.fbits");
                } else if (vt != i32Ty) {
                    v = builder->CreateIntCast(v, i32Ty, /*isSigned=*/false);
                }
                // Per-element type-directed: a mixed `spec:[3, 1.5f]` packs each
                // slot by its own type (int → value word, float → bit pattern).
                specVals.push_back(v);
            }
        }

        if (!specVals.empty()) {
            llvm::Type* specArrTy =
                llvm::ArrayType::get(i32Ty, specVals.size());
            llvm::Value* specArr =
                builder->CreateAlloca(specArrTy, nullptr, "xpu.spec");
            for (unsigned i = 0; i < specVals.size(); ++i) {
                llvm::Value* slot = builder->CreateInBoundsGEP(
                    specArrTy, specArr,
                    {llvm::ConstantInt::get(i64Ty, 0),
                     llvm::ConstantInt::get(i64Ty, i)}, "xpu.spec.slot");
                builder->CreateStore(specVals[i], slot);
            }
            llvm::Value* specBase = builder->CreateInBoundsGEP(
                specArrTy, specArr,
                {llvm::ConstantInt::get(i64Ty, 0),
                 llvm::ConstantInt::get(i64Ty, 0)}, "xpu.spec.base");

            // void __cajeta_xpu_launch_v3(i8* name, gx,gy,gz, bx,by,bz,
            //   i32 sharedBytes, ptr argv, i64 streamHandle, i32 deviceId,
            //   i32 specCount, ptr specValues)
            llvm::Function* launchV3 =
                module->getRuntimeFunction("__cajeta_xpu_launch_v3");
            if (!launchV3) {
                llvm::FunctionType* ft = llvm::FunctionType::get(
                    llvm::Type::getVoidTy(ctx),
                    {ptrTy, i32Ty, i32Ty, i32Ty, i32Ty, i32Ty, i32Ty, i32Ty,
                     ptrTy, i64Ty, i32Ty, i32Ty, ptrTy},
                    /*vararg=*/false);
                launchV3 = llvm::cast<llvm::Function>(
                    module->getLlvmModule()
                        ->getOrInsertFunction("__cajeta_xpu_launch_v3", ft)
                        .getCallee());
            }
            builder->CreateCall(
                launchV3,
                {nameStr, grid[0], grid[1], grid[2], block[0], block[1],
                 block[2], sharedBytes, argvBase, streamHandle,
                 llvm::ConstantInt::get(i32Ty, (uint64_t) -1),     // deviceId
                 llvm::ConstantInt::get(i32Ty, specVals.size()),    // specCount
                 specBase});
            return nullptr;  // launch is a void statement
        }

        // void __cajeta_xpu_launch(i8* name, i32 gridX, i32 gridY, i32 gridZ,
        //                          i32 blockX, i32 blockY, i32 blockZ,
        //                          i32 sharedBytes, ptr argv, i64 streamHandle)
        llvm::Function* launchFn =
            module->getRuntimeFunction("__cajeta_xpu_launch");
        if (!launchFn) {
            llvm::FunctionType* ft = llvm::FunctionType::get(
                llvm::Type::getVoidTy(ctx),
                {ptrTy, i32Ty, i32Ty, i32Ty, i32Ty, i32Ty, i32Ty, i32Ty, ptrTy,
                 i64Ty},
                /*vararg=*/false);
            launchFn = llvm::cast<llvm::Function>(
                module->getLlvmModule()
                    ->getOrInsertFunction("__cajeta_xpu_launch", ft)
                    .getCallee());
        }
        builder->CreateCall(launchFn,
                            {nameStr, grid[0], grid[1], grid[2],
                             block[0], block[1], block[2], sharedBytes,
                             argvBase, streamHandle});
        return nullptr;  // launch is a void statement
    }

} // namespace cajeta
