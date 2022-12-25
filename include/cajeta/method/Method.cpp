//
// Created by James Klappenbach on 2/19/22.
//

#include "cajeta/method/Method.h"
#include "cajeta/type/CajetaStructure.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/exception/VariableAssignmentException.h"
#include "cajeta/asn/DefaultBlock.h"
#include "cajeta/type/FormalParameter.h"

using namespace std;

#define ERROR_CAUSE_ASSIGNMENT_FINAL        "the field is declared final"
#define ERROR_CAUSE_VARIABLE_NOT_FOUND      "the field was not declared"
#define ERROR_CAUSE_VARIABLE_DUPLICATE      "the field name already exists"
#define ERROR_ID_ASSIGNMENT_FINAL           "CAJETA_ERROR_FINAL_ASSIGNMENT"
#define ERROR_ID_VARIABLE_NOT_FOUND         "CAJETA_ERROR_VARIABLE_NOT_FOUND"
#define ERROR_ID_VARIABLE_DUPLICATE         "CAJETA_ERROR_VARIABLE_DUPLICATE"

namespace cajeta {
    map<string, Method*> Method::archive;

    Method::Method(CajetaModule* module,
        string& name,
        CajetaType* returnType,
        vector<FormalParameter*>& parameterList,
        Block* block,
        CajetaStructure* parent) {
        this->module = module;
        this->parent = parent;
        this->name = name;
        this->returnType = returnType;
        this->parameterList = parameterList;
        this->block = block;
        constructor = parent->getQName()->getTypeName() == name;
        module->getScopeStack().push_back(parent->getScope());
        vector<llvm::Type*> llvmTypes;

        for (auto& parameter: parameterList) {
            parameter->setParent(this);
            llvmTypes.push_back(parameter->getType()->getLlvmType());
            parameters[parameter->getName()] = parameter;
        }

        // No default structure scope (class members) for static methods
        if (modifiers.find(STATIC) == modifiers.end()) {
            module->getScopeStack().push_back(parent->getScope());
        }

        llvmBasicBlock = nullptr;
    }

    /**
     * Default constructor method
     * @param name
     * @param returnType
     * @param parent
     */
    Method::Method(CajetaModule* module,
        string name,
        CajetaType* returnType,
        CajetaStructure* parent) {
        this->module = module;
        this->parent = parent;
        this->name = name;
        this->returnType = returnType;
        constructor = parent->getQName()->getTypeName() == name;
        block = new DefaultBlock();
        module->getScopeStack().push_back(parent->getScope());
        llvmBasicBlock = nullptr;
    }

    map<string, Method*>& Method::getArchive() { return archive; }

    void Method::setBlock(Block* block) {
        this->block = block;
    }

    void Method::createLocalVariable(CajetaModule* module, Field* field) {
        Scope* scope = module->getScopeStack().back();
        if (scope->getField(field->getName()) != nullptr) {
            throw VariableAssignmentException(field->getName(),
                field->getType()->getQName()->toCanonical(),
                ERROR_CAUSE_VARIABLE_DUPLICATE,
                ERROR_ID_VARIABLE_DUPLICATE);
        }
        field->getOrCreateAllocation(module);
        scope->putField(field);
    }

    void Method::setLocalVariable(CajetaModule* module, string name, llvm::Value* value) {
        Scope* scope = module->getScopeStack().back();
        Field* field = scope->getField(name);
        if (field == nullptr) {
            throw VariableAssignmentException(name,
                field->getType()->getQName()->toCanonical(),
                ERROR_CAUSE_VARIABLE_NOT_FOUND,
                ERROR_ID_VARIABLE_NOT_FOUND);
        }
        if (field->getModifiers().find(FINAL) != field->getModifiers().end()) {
            throw VariableAssignmentException(name,
                field->getType()->getQName()->toCanonical(),
                ERROR_CAUSE_ASSIGNMENT_FINAL,
                ERROR_ID_ASSIGNMENT_FINAL);
        }
        module->getBuilder()->CreateStore(value, field->getOrCreateAllocation(module));
    }


    void Method::generatePrototype() {
        vector<llvm::Type*> llvmTypes;

        bool staticMethod = modifiers.find(STATIC) != modifiers.end();

        if (!staticMethod) {
            auto thisParam = new FormalParameter(std::move("this"), parent);
            thisParam->setParent(this);
            parameterList.push_back(thisParam);
            parameters[thisParam->getName()] = thisParam;
        }

        for (auto formalParameter: parameterList) {
            llvmTypes.push_back(formalParameter->getType()->getLlvmType());
        }
        if (llvmTypes.size()) {
            llvmFunctionType = llvm::FunctionType::get(returnType->getLlvmType(), llvmTypes, false);
        } else {
            llvmFunctionType = llvm::FunctionType::get(returnType->getLlvmType(), false);
        }

        canonical = Method::buildCanonical(parent, name, parameterList);
        llvmFunction = llvm::Function::Create(llvmFunctionType, llvm::Function::ExternalLinkage,
            toCanonical(), module->getLlvmModule());
        string all;
        for (auto& fn: module->getLlvmModule()->getFunctionList()) {
            cout << fn.getName().str();
            all.append(fn.getName().str()).append(",");
        }

        archive[canonical] = this;

        module->getLlvmModule()->getOrInsertFunction(canonical, llvmFunctionType);
    }

    void Method::generateCode() {
        if (llvmBasicBlock == nullptr) {
            llvmBasicBlock = llvm::BasicBlock::Create(*module->getLlvmContext(), toCanonical(), llvmFunction);
            builder = new llvm::IRBuilder<>(llvmBasicBlock, llvmBasicBlock->begin());
            builder->SetInsertPoint(llvmBasicBlock);
            module->setBuilder(builder);
            module->setCurrentMethod(this);

            createScope();

            // TODO Fix this!  This is the base class stack frame initialization
            for (auto& arg: llvmFunction->args()) {
                llvm::AllocaInst* paramAlloca = builder->CreateAlloca(arg.getType(), nullptr, arg.getName());
                builder->CreateStore(&arg, paramAlloca);
                //    scopes.back()->fields[arg.getName().str()] = paramAlloca;
            }
            block->generateCode(module);

            destroyScope();

            module->getBuilder()->CreateRetVoid();
        }
    }

    void Method::createScope() {
        module->getScopeStack().push_back(new Scope(toCanonical(), module));
    }

    void Method::putScope(Field* field) {
        module->getScopeStack().back()->putField(field);
    }

    void Method::destroyScope() {
        Scope* scope = module->getScopeStack().back();
        delete scope;
        module->getScopeStack().pop_back();
    }

    Field* Method::getVariable(string name) {
        Scope* scope = module->getScopeStack().back();
        return scope->getField(name);
    }


    string Method::buildCanonical(CajetaStructure* parent, const string& name, vector<CajetaType*>& parameters) {
        string canonical;
        canonical.append(parent->toCanonical());
        canonical.append("::");
        canonical.append(name);
        canonical.append("(");

        if (!parameters.empty()) {
            bool first = true;
            for (auto& parameter: parameters) {
                if (first) {
                    first = false;
                } else {
                    canonical.append(",");
                }
                canonical.append(parameter->toCanonical());
            }
        }

        canonical.append(")");
        return canonical;
    }

    string Method::buildCanonical(CajetaStructure* parent, const string& name, vector<FormalParameter*>& parameters) {
        string canonical;
        canonical.append(parent->toCanonical());
        canonical.append("::");
        canonical.append(name);
        canonical.append("(");

        if (!parameters.empty()) {
            bool first = true;
            for (auto& parameter: parameters) {
                if (first) {
                    first = false;
                } else {
                    canonical.append(",");
                }
                canonical.append(parameter->getType()->toCanonical());
            }
        }

        canonical.append(")");
        return canonical;
    }

    string Method::buildCanonical(CajetaStructure* parent, const string& name, vector<llvm::Value*>& parameters) {
        string canonical;
        canonical.append(parent->toCanonical());
        canonical.append("::");
        canonical.append(name);
        canonical.append("(");

        if (!parameters.empty()) {
            bool first = true;
            for (auto& parameter: parameters) {
                if (first) {
                    first = false;
                } else {
                    canonical.append(",");
                }
                canonical.append(CajetaType::fromValue(parameter, parent)->toCanonical());
            }
        }

        canonical.append(")");
        return canonical;
    }
}