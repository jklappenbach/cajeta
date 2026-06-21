//
// SPIR-V shader interface variables — see header.
//

#include "SpirvInterface.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"

namespace cajeta {
namespace xpu {
namespace vulkan {

llvm::GlobalVariable* createInterfaceVar(llvm::Module& m, llvm::Type* ty,
                                         InterfaceStorage sc,
                                         SpirvDecoration decoration,
                                         uint32_t operand,
                                         const std::string& name) {
    llvm::LLVMContext& ctx = m.getContext();
    const bool isInput = (sc == InterfaceStorage::Input);

    // One !spirv.Decorations pair {i32 decoration, i32 operand}, wrapped in the
    // outer node the backend reads off the global.
    llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Metadata* pair[2] = {
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(i32, static_cast<uint32_t>(decoration))),
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i32, operand)),
    };
    llvm::MDNode* decor = llvm::MDNode::get(ctx, pair);

    // Input vars are read-only (`constant`), Output vars writable (`global`);
    // both are external hidden thread_local externally-initialized, matching
    // LLVM's position.{vs,ps}.ll interface-variable form.
    auto* gv = new llvm::GlobalVariable(
        m, ty, /*isConstant=*/isInput, llvm::GlobalValue::ExternalLinkage,
        /*Initializer=*/nullptr, name, /*InsertBefore=*/nullptr,
        llvm::GlobalValue::GeneralDynamicTLSModel,
        static_cast<unsigned>(sc), /*isExternallyInitialized=*/true);
    gv->setVisibility(llvm::GlobalValue::HiddenVisibility);
    gv->setMetadata("spirv.Decorations",
                    llvm::MDNode::get(ctx,
                        llvm::ArrayRef<llvm::Metadata*>{decor}));
    return gv;
}

llvm::GlobalVariable* createLocationVar(llvm::Module& m, llvm::Type* ty,
                                        InterfaceStorage sc, uint32_t location,
                                        const std::string& name) {
    return createInterfaceVar(m, ty, sc, SpirvDecoration::Location, location,
                              name);
}

llvm::GlobalVariable* createBuiltInVar(llvm::Module& m, llvm::Type* ty,
                                       InterfaceStorage sc, SpirvBuiltIn builtIn,
                                       const std::string& name) {
    return createInterfaceVar(m, ty, sc, SpirvDecoration::BuiltIn,
                              static_cast<uint32_t>(builtIn), name);
}

} // namespace vulkan
} // namespace xpu
} // namespace cajeta
