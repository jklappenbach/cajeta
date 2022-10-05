//
// Created by James Klappenbach on 10/3/22.
//

#include "cajeta/StructType.h"
#include "cajeta/Field.h"
#include "cajeta/Method.h"

namespace cajeta {
    llvm::Type* StructType::getLlvmType(llvm::LLVMContext* context) {
        if (llvmType == nullptr) {
            std::vector<llvm::Type*> llvmTypes;

            for (auto & field : fields) {
                llvm::Type* type = field.second->getType()->getLlvmType(context);
                llvmTypes.push_back(type);
            }
            llvmType = llvm::StructType::create(
                    *context,
                    llvm::ArrayRef<llvm::Type*>(llvmTypes),
                    qName->getTypeName(),
                    false);
        }
        return llvmType;
    }
}