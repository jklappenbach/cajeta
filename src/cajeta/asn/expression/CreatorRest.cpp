//
// Created by James Klappenbach on 4/19/23.
//

#include "CreatorRest.h"
#include "cajeta/compile/CajetaModule.h"

namespace cajeta {
    CreatorRest* CreatorRest::fromContext(CajetaParser::CreatorContext* ctx, antlr4::Token* token) {
        if (ctx->classCreatorRest()) {
            return new ClassCreatorRest(ctx->classCreatorRest(), token);
        } else {
            return new ArrayCreatorRest(ctx->arrayCreatorRest(), token);
        }
    }

    /**
     * Match the parameters provided to a constructor.  Put the constructor (Method*) in the pModule, which will then be
     * called when the Method regains control.
     *
     * @param module
     * @return
     */
    llvm::Value* ClassCreatorRest::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());
    //        list<CajetaTypePtr> types;
    //        vector<ParameterEntry> parameterEntries;
    //        FieldPtr currentField = pModule->getFieldStack().back();
    //        llvm::Value* thisValue = currentField->getOrCreateAllocation(pModule);
    //
    //        for (auto& param: parameters) {
    //            llvm::Value* value = param.expression->generateCode(pModule);
    //            parameterEntries.push_back(ParameterEntry(CajetaType::of(value), param.label, value));
    //        }
    //
    //        CajetaClassPtr structure = (CajetaStructure*) currentField->getType();
    //        string constructorName = Method::buildCanonical(structure,
    //            currentField->getType()->getQName()->getTypeName(), parameterEntries, true);
    //        structure->invokeMethod(constructorName, parameterEntries, true, thisValue);
        module->getAsnStack().pop_back();
        return nullptr;
    }

    /**
     * The initializer here will have expressions that will resolve to the llvmDimensions that will be allocated.
     *
     * @param module
     * @return
     */
    llvm::Value* ArrayCreatorRest::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());
    //        vector<llvm::Constant*> dimensionValues;
    //        llvm::Value* load = pModule->getCurrentValue();
    //        FieldPtr field = pModule->getFieldStack().back();
    //        CajetaArrayPtr arrayType = (CajetaArrayPtr) field->getType();
    //        auto& dataLayout = pModule->getLlvmModule()->getDataLayout();
    //        CajetaTypePtr int64Type = CajetaType::of("int64");
    //        llvm::Constant* allocSize = llvm::ConstantInt::get(int64Type->getLlvmType(),
    //            dataLayout.getTypeAllocSize(arrayType->getElementType()->getLlvmType()));
    //
    //        int ordinal = 1;
    //        char buffer[256];
    //        for (auto& node: children) {
    //            snprintf(buffer, 255, "#dim%d", ordinal);
    //            // TODO: These should probably be property fields!
    //            LocalFieldPtr field = new LocalField(string(buffer), int64Type, field);
    //            llvm::Constant* dimensionValue = (llvm::Constant*) node->generateCode(pModule);
    //            llvm::Value* allocation = pModule->getBuilder()->CreateStructGEP(arrayType->getLlvmType(),
    //                load, ordinal++);
    //            pModule->getScopeStack().peek()->putField(field);
    //            pModule->getBuilder()->CreateStore(dimensionValue, allocation);
    //            field->setAllocation(allocation);
    //            allocSize = llvm::ConstantExpr::getMul(dimensionValue, allocSize);
    //            dimensionValues.push_back(dimensionValue);
    //        }
    //
    //        llvm::Value* allocation = pModule->getBuilder()->CreateStructGEP(arrayType->getLlvmType(), load, 0);
    //        llvm::Instruction* mallocInst = MemoryManager::createMallocInstruction(pModule, allocSize,
    //            pModule->getBuilder()->GetInsertBlock());
    //        LocalFieldPtr arrayField = new StructureField("#array", arrayType->getElementType()->toPointerType(),
    //            pModule->getBuilder()->CreateStore(mallocInst, allocation), 0, field);
    //        pModule->getScopeStack().peek()->putField(arrayField);
        module->getAsnStack().pop_back();

        return nullptr;
    }

} // code