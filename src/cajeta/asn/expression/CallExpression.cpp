//
// Postfix call applied to the result of an expression.
//

#include "CallExpression.h"
#include "Identifier.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/error/Exception.h"

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
    //   kernel.launch(stream, grid: [gx], block: [bx])(args...)
    // to a host-side runtime call:
    //   __cajeta_xpu_launch(i8* kernelName, i32 gridX, i32 blockX, ptr argv)
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

        // First element of a `grid:`/`block:` dimension array, as i32.
        auto lowerDim = [&](const ExpressionPtr& dimExpr) -> llvm::Value* {
            ExpressionPtr e = dimExpr;
            if (auto arr =
                    std::dynamic_pointer_cast<ArrayLiteralExpression>(dimExpr)) {
                if (arr->getElements().empty()) return nullptr;
                e = arr->getElements()[0];
            }
            llvm::Value* v = e->generateCode(module);
            v = loadIfLValue(module, v, e);
            if (v->getType() != i32Ty) {
                v = builder->CreateIntCast(v, i32Ty, /*isSigned=*/false);
            }
            return v;
        };

        llvm::Value* gridX = nullptr;
        llvm::Value* blockX = nullptr;
        for (auto& p : callee->getParameters()) {
            std::string label = stripColon(p.label);
            if (label == "grid")  gridX = lowerDim(p.expression);
            else if (label == "block") blockX = lowerDim(p.expression);
            // The unlabeled first param is the stream — accepted, not yet plumbed.
        }
        if (!gridX || !blockX) {
            throw Exception("launch requires grid: and block: dimensions",
                            "XPU-N02");
        }

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

            llvm::Value* slot;
            if (isBuffer) {
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

        // void __cajeta_xpu_launch(i8* name, i32 gridX, i32 blockX, ptr argv)
        llvm::Function* launchFn =
            module->getRuntimeFunction("__cajeta_xpu_launch");
        if (!launchFn) {
            llvm::FunctionType* ft = llvm::FunctionType::get(
                llvm::Type::getVoidTy(ctx), {ptrTy, i32Ty, i32Ty, ptrTy},
                /*vararg=*/false);
            launchFn = llvm::cast<llvm::Function>(
                module->getLlvmModule()
                    ->getOrInsertFunction("__cajeta_xpu_launch", ft)
                    .getCallee());
        }
        builder->CreateCall(launchFn, {nameStr, gridX, blockX, argvBase});
        return nullptr;  // launch is a void statement
    }

} // namespace cajeta
