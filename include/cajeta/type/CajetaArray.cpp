//
// Created by James Klappenbach on 10/2/22.
//

#include "cajeta/type/CajetaArray.h"

namespace cajeta {
    void CajetaArray::generateSignature(CajetaModule* module) {
        vector<llvm::Type*> llvmMembers;
        for (auto &fieldEntry : fields) {
            llvmMembers.push_back(fieldEntry.second->getType()->getLlvmType());
        }
        ((llvm::StructType*) llvmType)->setBody(llvm::ArrayRef<llvm::Type*>(llvmMembers), false);

        ensureDefaultConstructor(module);

        for (auto methodEntry : methods) {
            methodEntry.second->generatePrototype(module);
        }
    }
    void CajetaArray::generateCode(CajetaModule* module) {
        for (auto methodEntry : methods) {
            methodEntry.second->generateCode(module);
        }
    }
}