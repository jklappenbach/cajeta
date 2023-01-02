//
// Created by James Klappenbach on 3/19/22.
//

#include "cajeta/asn/Expression.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/util/LiteralUtils.h"
#include "cajeta/util/MemoryManager.h"
#include "cajeta/type/CajetaArray.h"
#include "cajeta/field/LocalField.h"
#include "cajeta/field/LocalPropertyField.h"

namespace cajeta {
    Expression* Expression::fromContext(CajetaParser::ExpressionContext* ctx) {
        antlr4::Token* token = ctx->getStart();
        Expression* result = nullptr;
        if (ctx->ASSIGN()) {
            result = new BinaryOpExpression(BINARY_OP_ASSIGN, token);
        } else if (ctx->primary()) {
            result = PrimaryExpression::fromContext(ctx->primary());
        } else if (ctx->methodCall()) {

        } else if (ctx->NEW()) {
            result = new NewExpression(ctx->creator(), token);
        } else if (ctx->DOT()) {
            result = new DotExpression(ctx, token);
        } else if (ctx->identifier()) {
            result = new IdentifierExpression(ctx->identifier(), ctx->primary() != nullptr);
        } else if (ctx->LPAREN()) {
        } else if (ctx->LBRACK()) {
            result = new ArrayIndexExpression(ctx, token);
        } else if (!ctx->annotation().empty()) {

        } else if (ctx->creator()) {
        } else if (!ctx->typeType().empty()) {

        } else if (ctx->RPAREN()) {

        } else if (!ctx->BITAND().empty()) {

        } else if (ctx->AND()) {

        } else if (ctx->SUB()) {

        } else if (ctx->INC()) {
            if (ctx->prefix) {
                result = new PrefixExpression(PREFIX_OP_INC, token);
            } else {
                result = new PostfixExpression(POSTFIX_OP_INC, token);
            }
        } else if (ctx->DEC()) {
            if (ctx->prefix) {
                result = new PrefixExpression(PREFIX_OP_DEC, token);
            } else {
                result = new PostfixExpression(POSTFIX_OP_DEC, token);
            }
        } else if (ctx->TILDE()) {

        } else if (ctx->BANG()) {

        } else if (ctx->lambdaExpression()) {

        } else if (ctx->switchExpression()) {

        } else if (ctx->identifier()) {

        } else if (ctx->typeArguments()) {

        } else if (ctx->classType()) {

        } else if (ctx->MUL()) {

        } else if (ctx->DIV()) {

        } else if (ctx->MOD()) {

        } else if (!ctx->LT().empty()) {

        } else if (!ctx->GT().empty()) {

        } else if (ctx->LE()) {

        } else if (ctx->GE()) {

        } else if (ctx->EQUAL()) {
            cout << "Hit!";
        } else if (ctx->NOTEQUAL()) {

        } else if (ctx->CARET()) {

        } else if (ctx->BITOR()) {

        } else if (ctx->AND()) {

        } else if (ctx->OR()) {

        } else if (ctx->COLON()) {

        } else if (ctx->QUESTION()) {

        } else if (ctx->ADD_ASSIGN()) {
            result = new BinaryOpExpression(BINARY_OP_ADD_EQUALS, token);
        } else if (ctx->SUB_ASSIGN()) {

        } else if (ctx->MUL_ASSIGN()) {

        } else if (ctx->DIV_ASSIGN()) {

        } else if (ctx->AND_ASSIGN()) {

        } else if (ctx->OR_ASSIGN()) {

        } else if (ctx->XOR_ASSIGN()) {

        } else if (ctx->RSHIFT_ASSIGN()) {

        } else if (ctx->URSHIFT_ASSIGN()) {

        } else if (ctx->LSHIFT_ASSIGN()) {

        } else if (ctx->MOD_ASSIGN()) {

        } else if (ctx->THIS()) {

        } else if (ctx->innerCreator()) {

        } else if (ctx->SUPER()) {

        } else if (ctx->superSuffix()) {

        } else if (ctx->explicitGenericInvocation()) {

        } else if (ctx->nonWildcardTypeArguments()) {

        } else if (ctx->LBRACK()) {

        } else if (ctx->RBRACK()) {

        } else if (ctx->INSTANCEOF()) {

        } else if (ctx->pattern()) {
        }

        if (result) {
            if (!ctx->expression().empty()) {
                for (auto childContext: ctx->expression()) {
                    result->addChild(Expression::fromContext(childContext));
                }
            }
        }
        return result;
    }

    ArrayIndexExpression::ArrayIndexExpression(CajetaParser::ExpressionContext* ctx, antlr4::Token* token) : Expression(token) {

    }

    llvm::Value* ArrayIndexExpression::generateCode(CajetaModule* module) {
        llvm::Value* fieldAllocation = children[0]->generateCode(module);
        llvm::Constant* arrayIndex = nullptr;
        module->getTypeStack().push_back(CajetaType::of("int32"));
        for (int i = 1; i < children.size(); i++) {
            llvm::Constant* dimensionValue = (llvm::Constant*) children[i]->generateCode(module);
            if (arrayIndex == nullptr) {
                arrayIndex = dimensionValue;
            } else {
                arrayIndex = llvm::ConstantExpr::getMul(dimensionValue, arrayIndex);
            }
        }
        module->getTypeStack().pop_back();
        Field* currentField = module->getFieldStack().back();
        CajetaArray* type = (CajetaArray*) currentField->getType();
        llvm::Value* arrayPtr = module->getBuilder()->CreateStructGEP(type->getLlvmType(), fieldAllocation, 0);
        llvm::Value* arrayAllocation = module->getBuilder()->CreateLoad(type->getLlvmType()->getPointerTo(), arrayPtr);
        vector<llvm::Value*> indexes;
        indexes.push_back(arrayIndex);
        module->getTypeStack().push_back(type->getElementType());
        return module->getBuilder()->CreateGEP(type->getElementType()->getLlvmType(), arrayAllocation,
            llvm::ArrayRef<llvm::Value*>(indexes), "", true);
    }

//    enum LiteralType {
//        LITERAL_TYPE_BOOL,
//        LITERAL_TYPE_CHAR,
//        LITERAL_TYPE_NULL,
//        LITERAL_TYPE_STRING,
//        LITERAL_TYPE_TEXT_BLOCK,
//        LITERAL_TYPE_INTEGER,
//        LITERAL_TYPE_FLOAT
//    };

    LiteralExpression* LiteralExpression::fromContext(CajetaParser::LiteralContext* ctx) {
        if (ctx->integerLiteral()) {
            return new IntegerLiteralExpression(ctx->integerLiteral());
        } else if (ctx->floatLiteral()) {
            return new FloatLiteralExpression(ctx->floatLiteral());
        } else {
            return new TextLiteralExpression(ctx);
        }
    }

    llvm::Value* IntegerLiteralExpression::generateCode(CajetaModule* module) {
        unsigned bits = 64;
        uint8_t radix;

        CajetaType* type = module->getTypeStack().back();
        if (type->getStructType() == STRUCT_TYPE_PRIMITIVE) {
            bits = type->getLlvmType()->getIntegerBitWidth();
        }

        switch (integerLiteralType) {
            case INTEGER_LITERAL_TYPE_BINARY:
                radix = 2;
                break;
            case INTEGER_LITERAL_TYPE_OCT:
                radix = 8;
                break;
            case INTEGER_LITERAL_TYPE_DECIMAL:
                radix = 10;
                break;
            case INTEGER_LITERAL_TYPE_HEX:
                radix = 16;
                break;
        }
        return llvm::ConstantInt::getIntegerValue(type->getLlvmType(), llvm::APInt(bits, value, radix));
    }

    DotExpression::DotExpression(CajetaParser::ExpressionContext* ctx, antlr4::Token* token) : Expression(token) {
        identifier = ctx->identifier()->getText();
    }

    llvm::Value* DotExpression::generateCode(CajetaModule* module) {
        llvm::Value* value = children[0]->generateCode(module);
        CajetaStructure* structure = (CajetaStructure*) module->getFieldStack().back()->getType();
        module->getFieldStack().pop_back();
        ClassProperty* property = structure->getProperties()[identifier];
        // TODO: We shouldn't be using structureField, we should be creating a PropertyField from the property
        // TODO: We should automatically create an entry in our scope for a structure hierarchy.  These can be lazily loaded, but should be available for reference
        // TODO: and deallocation here.
        return nullptr;
//        module->getFieldStack().push_back(structureField);
//        if (structureField == nullptr) {
//            throw "identifier did not map to field";
//        }
//        return module->getBuilder()->CreateStructGEP(structure->getLlvmType(),
//                                                     value,
//                                                     structureField->getOrder(),
//                                                     identifier);
    }

    llvm::Value* BinaryOpExpression::generateCode(CajetaModule* module) {
        if (binaryOp == BINARY_OP_ASSIGN) {
            llvm::Value* destination = children[0]->generateCode(module);
            llvm::Value* source = children[1]->generateCode(module);
            module->getTypeStack().pop_back();
            module->getFieldStack().pop_back();
            return module->getBuilder()->CreateStore(source, destination);
        }
        return nullptr;
    }


//    antlr4::tree::TerminalNode *LPAREN();
//    ExpressionContext *expression();
//    antlr4::tree::TerminalNode *RPAREN();
//    antlr4::tree::TerminalNode *THIS();
//    antlr4::tree::TerminalNode *SUPER();
//    LiteralContext *literal();
//    IdentifierContext *identifier();
//    TypeTypeOrVoidContext *typeTypeOrVoid();
//    antlr4::tree::TerminalNode *DOT();
//    antlr4::tree::TerminalNode *CLASS();
//    NonWildcardTypeArgumentsContext *nonWildcardTypeArguments();
//    ExplicitGenericInvocationSuffixContext *explicitGenericInvocationSuffix();
//    ArgumentsContext *arguments();
    Expression* PrimaryExpression::fromContext(CajetaParser::PrimaryContext* ctx) {
        Expression* result = nullptr;
        if (ctx->LPAREN()) {
            result = Expression::fromContext(ctx->expression());
        } else if (ctx->literal()) {
            result = LiteralExpression::fromContext(ctx->literal());
        } else if (ctx->identifier()) {
            result = new IdentifierExpression(ctx->identifier(), true);
        } else if (ctx->THIS()) {
            result = new ThisExpression(ctx->expression());
        }
        return result;
    }

    llvm::Value* PrimaryExpression::generateCode(CajetaModule* module) {
        return nullptr;
    }

    llvm::Value* IdentifierExpression::generateCode(CajetaModule* module) {
        if (primary) {
            Field* field = module->getCurrentMethod()->getVariable(identifier);
            module->getFieldStack().push_back(field);
            return field->createLoad(module);
        } else {
            llvm::Value* value = module->getValueStack().back();
            CajetaStructure* structure;
            try {
                structure = (CajetaStructure*) CajetaType::getCanonicalMap()[value->getType()->getStructName().str()];
            } catch (exception) {
                throw "bad type";
            }
            ClassProperty* structureField = structure->getProperties()[identifier];
            return module->getBuilder()->CreateStructGEP(structureField->getType()->getLlvmType(),
                module->getValueStack().back(),
                structureField->getOrder(),
                identifier);
        }
    }

    CreatorRest* CreatorRest::fromContext(CajetaParser::CreatorContext* ctx, antlr4::Token* token) {
        if (ctx->classCreatorRest()) {
            return new ClassCreatorRest(ctx->classCreatorRest(), token);
        } else {
            return new ArrayCreatorRest(ctx->arrayCreatorRest(), token);
        }
    }

    /**
     * Match the parameters provided to a constructor.  Put the constructor (Method*) in the module, which will then be
     * called when the Method regains control.
     *
     * @param module
     * @return
     */
    llvm::Value* ClassCreatorRest::generateCode(CajetaModule* module) {
        list<CajetaType*> types;
        vector<llvm::Value*> parameterValues;
        Field* currentField = module->getFieldStack().back();
        llvm::Value* thisValue = currentField->getOrCreateAllocation(module);
        parameterValues.push_back(thisValue);

        for (auto& node: children) {
            parameterValues.push_back(node->generateCode(module));
        }

        string constructorName = Method::buildCanonical((CajetaStructure*) currentField->getType(),
            currentField->getType()->getQName()->getTypeName(), parameterValues);

        Method* constructor = Method::getArchive()[constructorName];

        if (constructor == nullptr) {
            throw "A constructor with the specified signature could not be found.";
        } else {
            module->getBuilder()->CreateCall(constructor->getLlvmFunctionType(),
                constructor->getLlvmFunction(), parameterValues);
        }
        return nullptr;
    }

    /**
     * The initializer here will have expressions that will resolve to the dimensions that will be allocated.
     *
     * @param module
     * @return
     */
    llvm::Value* ArrayCreatorRest::generateCode(CajetaModule* module) {
        vector<llvm::Constant*> dimensionValues;
        Field* currentField = module->getFieldStack().back();
        llvm::Value* load = currentField->createLoad(module);
        CajetaArray* arrayType = (CajetaArray*) currentField->getType();
        auto& dataLayout = module->getLlvmModule()->getDataLayout();
        CajetaType* int64Type = CajetaType::of("int64");
        llvm::Constant* allocSize = llvm::ConstantInt::get(int64Type->getLlvmType(),
            dataLayout.getTypeAllocSize(arrayType->getElementType()->getLlvmType()));

        int ordinal = 1;
        char buffer[256];
        for (auto& node: children) {
            snprintf(buffer, 255, "#dim%d", ordinal);
            // TODO: These should probably be property fields!
            LocalField* field = new LocalField(string(buffer), int64Type, currentField);
            llvm::Constant* dimensionValue = (llvm::Constant*) node->generateCode(module);
            llvm::Value* allocation = module->getBuilder()->CreateStructGEP(arrayType->getLlvmType(),
                load, ordinal++);
            module->getCurrentMethod()->putScope(field);
            module->getBuilder()->CreateStore(dimensionValue, allocation);
            field->setAllocation(allocation);
            allocSize = llvm::ConstantExpr::getMul(dimensionValue, allocSize);
            dimensionValues.push_back(dimensionValue);
        }

        llvm::Value* allocation = module->getBuilder()->CreateStructGEP(arrayType->getLlvmType(), load, 0);
        llvm::Instruction* mallocInst = MemoryManager::createMallocInstruction(module, allocSize,
            module->getBuilder()->GetInsertBlock());
        LocalField* field = new LocalPropertyField("#array", arrayType->getElementType()->toPointerType(),
            module->getBuilder()->CreateStore(mallocInst, allocation), 0, currentField);
        module->getCurrentMethod()->putScope(field);
        return nullptr;
    }

    llvm::Value* NewExpression::generateCode(CajetaModule* module) {
        Field* field = module->getFieldStack().back();
        CajetaType* type = field->getType();
        llvm::Constant* allocSize = llvm::ConstantExpr::getSizeOf(type->getLlvmType());
        llvm::Instruction* mallocInst = MemoryManager::createMallocInstruction(module, allocSize,
            module->getBuilder()->GetInsertBlock());
        module->getBuilder()->CreateStore(mallocInst, field->getOrCreateAllocation(module));
        this->creatorRest->generateCode(module);
        return nullptr;
    }
}

