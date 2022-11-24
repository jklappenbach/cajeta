//
// Created by James Klappenbach on 2/19/22.
//

#include "cajeta/method/Method.h"
#include "cajeta/type/CajetaStructure.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/exception/VariableAssignmentException.h"
#include "cajeta/asn/DefaultBlock.h"

using namespace std;

#define ERROR_CAUSE_ASSIGNMENT_FINAL        "the field is declared final"
#define ERROR_CAUSE_VARIABLE_NOT_FOUND      "the field was not declared"
#define ERROR_CAUSE_VARIABLE_DUPLICATE      "the field name already exists"
#define ERROR_ID_ASSIGNMENT_FINAL           "CAJETA_ERROR_FINAL_ASSIGNMENT"
#define ERROR_ID_VARIABLE_NOT_FOUND         "CAJETA_ERROR_VARIABLE_NOT_FOUND"
#define ERROR_ID_VARIABLE_DUPLICATE         "CAJETA_ERROR_VARIABLE_DUPLICATE"

namespace cajeta {
    map<string, Method*> Method::archive;

    Method::Method(string& name,
                   CajetaType* returnType,
                   vector<FormalParameter*>& parameters,
                   Block* block,
                   CajetaStructure* parent) {
        this->parent = parent;
        this->name = name;
        this->returnType = returnType;
        this->parameters = parameters;
        this->block = block;
        constructor = parent->getQName()->getTypeName() == name;
        scopes.push_back(parent->getScope());
        vector<llvm::Type*> llvmTypes;

        for (auto parameter : parameters) {
            llvmTypes.push_back(parameter->getType()->getLlvmType());
            this->parameters.push_back(parameter);
        }

        // No default structure scope (class members) for static methods
        if (modifiers.find(STATIC) == modifiers.end()) {
            scopes.push_back(parent->getScope());
        }
    }

    /**
     * Default constructor method
     * @param name
     * @param returnType
     * @param parent
     */
    Method::Method(string name,
                   CajetaType* returnType,
                   CajetaStructure* parent) {
        this->parent = parent;
        this->name = name;
        this->returnType = returnType;
        constructor = parent->getQName()->getTypeName() == name;
        block = new DefaultBlock();
        scopes.push_back(parent->getScope());
    }

    map<string, Method*>& Method::getArchive() { return archive; }

    void Method::setBlock(Block* block) {
        this->block = block;
    }

    void Method::createLocalVariable(CajetaModule* module, cajeta::Field* field) {
        Scope* scope = scopes.back();
        if (scope->fields.find(field->getName()) != scope->fields.end()) {
            throw VariableAssignmentException(field->getName(),
                                              field->getType()->getQName()->toCanonical(),
                                              ERROR_CAUSE_VARIABLE_DUPLICATE,
                                              ERROR_ID_VARIABLE_DUPLICATE);
        }
        field->getOrCreateAllocation(module);
        scope->fields[field->getName()] = field;
    }

    void Method::setLocalVariable(CajetaModule* module, string name, llvm::Value* value) {
        Scope* scope = scopes.back();
        Field* field = scope->fields[name];
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
        module->getBuilder()->CreateStore(value, field->getOrCreateStackAllocation(module));
    }


    void Method::generatePrototype(CajetaModule* module) {
        vector<llvm::Type*> llvmTypes;

        bool staticMethod = modifiers.find(STATIC) != modifiers.end();

        if (!staticMethod) {
            auto thisParam = new FormalParameter(std::move("this"), parent);
            parameters.push_back(thisParam);
        }

        for (auto formalParameter : parameters) {
            llvmTypes.push_back(formalParameter->getType()->getLlvmType());
        }
        if (llvmTypes.size()) {
            llvmFunctionType = llvm::FunctionType::get(returnType->getLlvmType(), llvmTypes, false);
        } else {
            llvmFunctionType = llvm::FunctionType::get(returnType->getLlvmType(), false);
        }

        canonical = Method::buildCanonical(parent, name, parameters);
        llvmFunction = llvm::Function::Create(llvmFunctionType, llvm::Function::ExternalLinkage,
                                              this->name, module->getLlvmModule());
        string all;
        for (auto &fn : module->getLlvmModule()->getFunctionList()) {
            cout << fn.getName().str();
            all.append(fn.getName().str()).append(",");
        }

        archive[canonical] = this;

        module->getLlvmModule()->getOrInsertFunction(canonical, llvmFunctionType);
    }

    void Method::generateCode(CajetaModule* module) {
        llvmBasicBlock = llvm::BasicBlock::Create(*module->getLlvmContext(), name, llvmFunction);
        builder = new llvm::IRBuilder<>(llvmBasicBlock, llvmBasicBlock->begin());
        builder->SetInsertPoint(llvmBasicBlock);
        module->setBuilder(builder);
        module->setCurrentMethod(this);

        createScope(module);
        block->generateCode(module);
        destroyScope();
        module->getBuilder()->CreateRetVoid();
    }

    void Method::createScope(CajetaModule* module) {
        scopes.push_back(new Scope(module));
    }

    string Method::buildCanonical(CajetaStructure* parent, const string& name, vector<CajetaType*>& parameters) {
        string canonical;
        canonical.append(parent->toCanonical());
        canonical.append("::");
        canonical.append(name);
        canonical.append("(");

        if (!parameters.empty()) {
            bool first = true;
            for (auto &parameter : parameters) {
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
            for (auto &parameter : parameters) {
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
            for (auto &parameter : parameters) {
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