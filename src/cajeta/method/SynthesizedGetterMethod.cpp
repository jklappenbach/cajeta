#include "SynthesizedGetterMethod.h"
#include "../type/CajetaClass.h"
#include "../type/CajetaView.h"
#include "../type/CajetaArray.h"
#include "../compile/CajetaModule.h"
#include "../error/Exception.h"

#include <llvm/IR/IRBuilder.h>

using namespace std;

namespace cajeta {

    SynthesizedGetterMethod::SynthesizedGetterMethod(
            CajetaModulePtr module, CajetaClassPtr parent,
            StructurePropertyPtr field)
        : Method(module, field->getName(), field->getType(), parent),
          field(field) {
        this->parent = parent;
        // Default block stays a DefaultBlock from the base ctor; we
        // override generateCode() entirely so the block is unused.
    }

    void SynthesizedGetterMethod::generateCode() {
        // Method::generatePrototype has already built llvmFunction with
        // signature (this) -> fieldType. Emit a single entry block that
        // GEPs the field, loads it, and returns.
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvmBasicBlock = llvm::BasicBlock::Create(ctx, "entry", llvmFunction);
        llvm::IRBuilder<> b(llvmBasicBlock);

        llvm::Value* thisPtr = llvmFunction->getArg(0);
        int idx = parent->getFieldLlvmIndex(field);
        if (idx < 0) {
            throw Exception(
                "@Getter synthesizer: field '" + field->getName()
                + "' has no LLVM index on '"
                + parent->getQName()->toCanonical() + "'",
                "CAJETA_ERROR_GETTER_FIELD_INDEX");
        }

        llvm::Value* fieldPtr = b.CreateStructGEP(
            parent->getLlvmType(), thisPtr, (unsigned) idx,
            std::string("get.") + field->getName());

        // Load at the field's STORAGE type per CajetaClass::generatePrototype's
        // fieldLayoutType: class refs and arrays store as `ptr`; views and
        // interfaces store inline; primitives store at their LLVM width.
        // The Method's return-type LLVM signature uses the same convention
        // (Method.cpp's class-pass-by-pointer rule), so the loaded value
        // matches the function's declared return type.
        llvm::Type* slotType;
        CajetaTypePtr ftype = field->getType();
        bool isArr = dynamic_pointer_cast<CajetaArray>(ftype) != nullptr;
        bool isView = dynamic_pointer_cast<CajetaView>(ftype) != nullptr;
        auto cls = dynamic_pointer_cast<CajetaClass>(ftype);
        bool isInterface = cls && cls->isInterface();
        bool isClassRef = cls && !isView && !isInterface;
        if (isArr || isClassRef) {
            slotType = llvm::PointerType::get(ctx, 0);
        } else {
            // Primitive, view-inline, or interface-fat-pointer-inline.
            slotType = ftype->getLlvmType();
        }
        llvm::Value* val = b.CreateLoad(
            slotType, fieldPtr, std::string("get.v.") + field->getName());
        b.CreateRet(val);
    }
}
