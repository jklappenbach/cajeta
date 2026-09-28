#include "ComponentProvideMethod.h"
#include "../type/CajetaClass.h"
#include "../type/CajetaType.h"
#include "../type/FormalParameter.h"
#include "../error/Exception.h"
#include "../asn/expression/Expression.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"

namespace cajeta {
    ComponentProvideMethod::ComponentProvideMethod(
        CajetaModulePtr module,
        CajetaClassPtr parent,
        CajetaModule::ComponentDescriptorPtr descriptor)
        : Method(module, std::string("__cajeta_provide"), CajetaType::of("void"), parent),
          descriptor(std::move(descriptor)) {
        llvmFunctionType = nullptr;
        llvmFunction = nullptr;
        addModifier(STATIC);
    }

    void ComponentProvideMethod::initParameters() {
        if (!parameterList.empty()) return;
        auto value = make_shared<FormalParameter>("value", parent);
        value->setTransferred(true);
        value->setParent(shared_from_this());
        parameterList.push_back(value);
        parameters[value->getName()] = value;
    }

    void ComponentProvideMethod::generateCode() {
        auto& llvmFunction = llvmFunctionRef();
        auto& ctx = *module->getLlvmContext();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Constant* nullPtr = llvm::ConstantPointerNull::get(
            llvm::cast<llvm::PointerType>(ptrTy));
        const std::string canonical = parent->getQName()->toCanonical();
        const std::string scopeName = descriptor->scope->qualified();

        llvm::BasicBlock* entry = llvm::BasicBlock::Create(ctx, "entry", llvmFunction);
        builder = new llvm::IRBuilder<>(entry);
        module->setBuilder(builder);
        module->setCurrentMethod(shared_from_this());

        llvm::Function* findFn = module->getRuntimeFunction("__cajeta_anchor_find");
        llvm::Function* acquireFn = module->getRuntimeFunction("__cajeta_anchor_acquire");
        llvm::Function* publishFn = module->getRuntimeFunction("__cajeta_anchor_publish");
        llvm::Function* dropFn = module->getRuntimeFunction("__cajeta_class_virtual_drop");
        if (!findFn || !acquireFn || !publishFn || !dropFn) {
            throw Exception("the runtime has no component-scope anchors", "CAJETA_ERROR_INTERNAL");
        }
        parent->patchVirtualTableDropFn();

        llvm::Value* value = llvmFunction->getArg(0);
        // The value's title, from the transfer word: a lent value is recorded, not owned.
        llvm::Value* owned = builder->getFalse();
        if (llvmFunction->arg_size() > 1) {
            llvm::Value* word = llvmFunction->getArg(llvmFunction->arg_size() - 1);
            owned = builder->CreateICmpNE(
                builder->CreateAnd(word, llvm::ConstantInt::get(i64Ty, 1)),
                llvm::ConstantInt::get(i64Ty, 0), "owned");
        }

        // Frees an owned value that is refused, then throws.
        auto refuse = [&](const std::string& exception, const std::string& message) {
            llvm::BasicBlock* freeBB = llvm::BasicBlock::Create(ctx, "free_refused", llvmFunction);
            llvm::BasicBlock* throwBB = llvm::BasicBlock::Create(ctx, "throw_refused", llvmFunction);
            builder->CreateCondBr(owned, freeBB, throwBB);
            builder->SetInsertPoint(freeBB);
            builder->CreateCall(dropFn, {value});
            builder->CreateBr(throwBB);
            builder->SetInsertPoint(throwBB);
            emitThrowStdlibException(module, exception, message);
        };

        llvm::Constant* scopeKey = CajetaModule::scopeKeyGlobal(llvmFunction->getParent(), scopeName);
        llvm::Constant* componentKey = CajetaModule::componentKeyGlobal(
            llvmFunction->getParent(), canonical);
        llvm::Value* frame = builder->CreateCall(findFn, {scopeKey}, "frame");
        llvm::BasicBlock* inactive = llvm::BasicBlock::Create(ctx, "inactive", llvmFunction);
        llvm::BasicBlock* active = llvm::BasicBlock::Create(ctx, "active", llvmFunction);
        builder->CreateCondBr(builder->CreateICmpEQ(frame, nullPtr), inactive, active);
        builder->SetInsertPoint(inactive);
        refuse("cajeta.error.ScopeNotActiveException",
               "component " + canonical + " is provided into scope \"" + scopeName
                   + "\", and no activation of it is active");

        builder->SetInsertPoint(active);
        llvm::IRBuilder<> eb(entry, entry->begin());
        llvm::AllocaInst* claim = eb.CreateAlloca(
            llvm::ArrayType::get(llvm::Type::getInt8Ty(ctx), 48), nullptr, "claim");
        claim->setAlignment(llvm::Align(8));
        llvm::Value* cur = builder->CreateCall(acquireFn, {frame, componentKey, claim}, "cur");
        llvm::BasicBlock* place = llvm::BasicBlock::Create(ctx, "place", llvmFunction);
        llvm::BasicBlock* taken = llvm::BasicBlock::Create(ctx, "taken", llvmFunction);
        builder->CreateCondBr(builder->CreateICmpEQ(cur, nullPtr), place, taken);
        builder->SetInsertPoint(taken);
        refuse("cajeta.error.ScopedBuildFailedException",
               "component " + canonical + " already has an instance in scope \""
                   + scopeName + "\"; provide it once per scope");

        builder->SetInsertPoint(place);
        llvm::Value* preDestroy = nullPtr;
        for (auto& [mkey, m] : parent->getMethods()) {
            if (!m || !m->findAnnotation("PreDestroy")) continue;
            if (m->getModifiers().count(STATIC)) continue;
            llvm::FunctionType* hookTy = m->getLlvmFunctionType();
            if (m->getLlvmFunction()) {
                preDestroy = CajetaModule::ensureFunctionVisible(builder, m->getLlvmFunction(),
                                                                 hookTy);
            }
            break;
        }
        builder->CreateCall(publishFn, {
            claim, value,
            builder->CreateSelect(owned, dropFn, nullPtr),
            builder->CreateSelect(owned, preDestroy, nullPtr)});
        builder->CreateRetVoid();
    }
}
