//
// Postfix call applied to the result of an expression.
//

#include "CallExpression.h"
#include "Identifier.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
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
    // The stream argument is accepted syntactically but not yet plumbed —
    // ordering is currently the default stream; Stream.sync() handles the
    // host/device barrier separately.
    llvm::Value* CallExpression::generateCode(CajetaModulePtr module) {
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
        for (auto& p : callee->getParameters()) {
            std::string label = stripColon(p.label);
            if (label == "grid")  haveGrid  = lowerDims(p.expression, grid);
            else if (label == "block") haveBlock = lowerDims(p.expression, block);
            else if (label == "sharedBytes") {
                llvm::Value* sb[3];
                if (lowerDims(p.expression, sb)) sharedBytes = sb[0];
            }
            // The unlabeled first param is the stream — accepted, not yet plumbed.
        }
        if (!haveGrid || !haveBlock) {
            throw Exception("launch requires grid: and block: dimensions",
                            "XPU-N02");
        }
        // `shared:` is the dynamic-shared-memory byte count (cuLaunchKernel
        // sharedMemBytes). Optional — kernels using only static shared memory
        // (or none) omit it; default 0.
        if (!sharedBytes) sharedBytes = llvm::ConstantInt::get(i32Ty, 0);

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
                klass->toCanonical().rfind("cajeta.xpu.core.Buffer", 0) == 0;
            // Texture2D (Item 8): marshalled exactly like a Buffer — its
            // deviceHandle (the runtime texture-object pointer / image handle)
            // flows through the kernelParams slot, and the launch borrows it.
            bool isTexture = klass &&
                klass->toCanonical().rfind("cajeta.xpu.core.Texture2D", 0) == 0;
            // AccelerationStructure (Part C): a descriptor-bound device BVH. It
            // marshals via the POD-by-value path below (its deviceHandle is the
            // first field), but the launch borrows it just like a Buffer/Texture2D.
            bool isAccel = xpu::isAccelStructType(argExpr->getResolvedType());

            // Launch borrow scope (CajetaXPU §3.5/§11): a launch borrows each
            // device-resource arg (Buffer / Texture2D / AccelerationStructure)
            // until the next Stream.sync() / Event.waitHost(); record it so a
            // free/reassign/drop-before-sync is caught (XPU-K02). Hoisted out of
            // the kind-specific marshalling below so AS (a POD-marshalled handle)
            // is covered too.
            if (isBuffer || isTexture || isAccel) {
                if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(argExpr)) {
                    if (auto sc = module->getScopeStack().peek()) {
                        sc->recordLaunchBorrow(id->getTextValue());
                    }
                }
            }

            llvm::Value* slot;
            if (isBuffer || isTexture) {
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

        // void __cajeta_xpu_launch(i8* name, i32 gridX, i32 gridY, i32 gridZ,
        //                          i32 blockX, i32 blockY, i32 blockZ,
        //                          i32 sharedBytes, ptr argv)
        llvm::Function* launchFn =
            module->getRuntimeFunction("__cajeta_xpu_launch");
        if (!launchFn) {
            llvm::FunctionType* ft = llvm::FunctionType::get(
                llvm::Type::getVoidTy(ctx),
                {ptrTy, i32Ty, i32Ty, i32Ty, i32Ty, i32Ty, i32Ty, i32Ty, ptrTy},
                /*vararg=*/false);
            launchFn = llvm::cast<llvm::Function>(
                module->getLlvmModule()
                    ->getOrInsertFunction("__cajeta_xpu_launch", ft)
                    .getCallee());
        }
        builder->CreateCall(launchFn,
                            {nameStr, grid[0], grid[1], grid[2],
                             block[0], block[1], block[2], sharedBytes,
                             argvBase});
        return nullptr;  // launch is a void statement
    }

} // namespace cajeta
