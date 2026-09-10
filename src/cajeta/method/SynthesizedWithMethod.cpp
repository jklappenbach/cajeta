#include "SynthesizedWithMethod.h"
#include "../type/CajetaClass.h"
#include "../type/CajetaView.h"
#include "../type/CajetaArray.h"
#include "../type/FormalParameter.h"
#include "../compile/CajetaModule.h"
#include "../error/Exception.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/DataLayout.h>

#include <cctype>

using namespace std;

namespace cajeta {

    static std::string buildWithMethodName(const std::string& fieldName) {
        if (fieldName.empty()) return "with";
        std::string out = "with";
        out += (char) std::toupper((unsigned char) fieldName[0]);
        if (fieldName.size() > 1) out.append(fieldName.substr(1));
        return out;
    }

    SynthesizedWithMethod::SynthesizedWithMethod(
            CajetaModulePtr module, CajetaClassPtr parent,
            StructurePropertyPtr field)
        : Method(module, buildWithMethodName(field->getName()),
                 std::static_pointer_cast<CajetaType>(parent),
                 parent),
          field(field) {
        this->parent = parent;
    }

    void SynthesizedWithMethod::initParameter() {
        if (!parameterList.empty()) return;  // idempotent
        auto valueParam = make_shared<FormalParameter>(
            field->getName(), field->getType());
        valueParam->setParent(shared_from_this());
        parameterList.push_back(valueParam);
        parameters[valueParam->getName()] = valueParam;
    }

    void SynthesizedWithMethod::generateCode() {
        auto& llvmFunction = llvmFunctionRef();  // U6.3b: frozen-aware
        // Idempotent: Phase 2 codegen loops until quiescent and may revisit this
        // method; a second visit would append a duplicate `entry` block, which the
        // JIT bitcode parse rejects.
        if (llvmFunction && !llvmFunction->empty()) return;
        // Signature post-prototype: (this, value) -> ptr, arg(0) this, arg(1) the value.
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvmBasicBlock = llvm::BasicBlock::Create(ctx, "entry", llvmFunction);
        llvm::IRBuilder<> b(llvmBasicBlock);
        llvm::Module* lmod = module->getLlvmModule();
        const llvm::DataLayout& dl = lmod->getDataLayout();

        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);

        llvm::Function* allocFn = module->getRuntimeFunction("__cajeta_alloc");
        if (!allocFn) {
            throw Exception(
                "@With synthesizer: runtime helper __cajeta_alloc not linked",
                "CAJETA_ERROR_WITH_RUNTIME");
        }

        // memcpy preserves the vtable pointer at slot 0 and every field bit-for-bit;
        // the targeted field is overwritten afterwards.
        llvm::Function* memcpyFn = llvm::Intrinsic::getOrInsertDeclaration(
            lmod, llvm::Intrinsic::memcpy,
            {ptrTy, ptrTy, i64Ty});

        llvm::Type* classTy = parent->getLlvmType();
        uint64_t instSize = dl.getTypeAllocSize(classTy);

        llvm::Value* thisPtr = llvmFunction->getArg(0);
        llvm::Value* newValue = llvmFunction->getArg(1);

        llvm::Value* sizeArg = llvm::ConstantInt::get(i64Ty, instSize);
        llvm::Value* newInst = b.CreateCall(allocFn, {sizeArg},
            std::string("with.alloc.") + field->getName());

        b.CreateCall(memcpyFn, {
            newInst,
            thisPtr,
            sizeArg,
            llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx), 0)
        });

        int idx = parent->getFieldLlvmIndex(field);
        if (idx < 0) {
            throw Exception(
                "@With synthesizer: field '" + field->getName()
                + "' has no LLVM index on '"
                + parent->getQName()->toCanonical() + "'",
                "CAJETA_ERROR_WITH_FIELD_INDEX");
        }
        llvm::Value* slot = b.CreateStructGEP(classTy, newInst, (unsigned) idx,
            std::string("with.slot.") + field->getName());
        b.CreateStore(newValue, slot);

        b.CreateRet(newInst);
    }
}
