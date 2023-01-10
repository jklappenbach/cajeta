//
// Created by James Klappenbach on 4/19/23.
//

#include "MethodCallExpression.h"
#include "cajeta/compile/CajetaModule.h"

namespace cajeta {
    /**
 * First, get the full name of the object.
 * @param module
 * @return
 */
    llvm::Value* MethodCallExpression::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());

        // TODO Find out if this is the correct approach
//        CajetaStructurePtr structure = static_pointer_cast<CajetaStructure>(module->getFieldStack().back()->getType());
//        llvm::Value* instance = nullptr;
//        if (module->getFieldStack().size() > 0) {
//            instance = module->getFieldStack().back()->getOrCreateAllocation();
//        }

//        vector<ParameterEntry> entries;
//
//        for (auto& param : parameters) {
//            llvm::Value* value = param.expression->generateCode(module);
//            ParameterEntry entry(CajetaType::of(value), param.label, value);
//            entries.push_back(entry);
//        }
//        structure->invokeMethod(methodCallName, entries, false, instance);
        module->getAsnStack().pop_back();
        return nullptr;
    }


} // cajeta