//
// Created by James Klappenbach on 3/19/22.
//

#include "cajeta/asn/Expression.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/util/LiteralUtils.h"
#include "cajeta/util/MemoryManager.h"

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
        } else if (ctx->LPAREN()) {

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

        } else if (ctx->DOT()) {

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
                for (auto childContext : ctx->expression()) {
                    result->addChild(Expression::fromContext(childContext));
                }
            }
        }
        return result;
    }

    CajetaType* Expression::toType(CajetaModule* module) { return CajetaType::of("void"); }


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

    CajetaType* LiteralExpression::toType(CajetaModule* module) {
        return module->getInitializerType();
    }

    llvm::Value* IntegerLiteralExpression::generateCode(CajetaModule* module) {
        unsigned bits = module->getInitializerType()->getLlvmType()->getPrimitiveSizeInBits();
        uint8_t radix;
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
        return llvm::ConstantInt::getIntegerValue(module->getInitializerType()->getLlvmType(), llvm::APInt(bits, value, radix));
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
            result = new IdentifierExpression(ctx->identifier());
        } else if (ctx->THIS()) {
            result = new ThisExpression(ctx->expression());
        }
        return result;
    }

    llvm::Value* PrimaryExpression::generateCode(CajetaModule* module) {
        return nullptr;
    }

    llvm::Value* IdentifierExpression::generateCode(CajetaModule* module) {
        Field* field = module->getCurrentMethod()->getLocalVariable(this->identifier);
        llvm::AllocaInst* alloca = (llvm::AllocaInst*) field->getOrCreateStackAllocation(module);
        return module->getBuilder()->CreateLoad(alloca->getAllocatedType(), alloca, field->getName());
    }

    CajetaType* IdentifierExpression::toType(CajetaModule* module) {
        Field* field = module->getCurrentMethod()->getLocalVariable(identifier);
        if (field != nullptr) {
            return field->getType();
        } else {
            throw "bad identifier"; // TODO: Create new exception for this.
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
        vector<llvm::Value*> values;
        Field* currentField = module->getCurrentField();

        types.push_back(module->getCurrentStructure());
        llvm::Value* thisValue = currentField->getAllocation();
        values.push_back(thisValue);

        for (auto& expression : expressionList) {
            types.push_back(expression->toType(module));
            values.push_back(expression->generateCode(module));
        }

        string canonical = Method::buildCanonical(
                module->getCurrentStructure(),
                module->getCurrentStructure()->getQName()->getTypeName(),
                types);
        Method* constructor = Method::getArchive()[canonical];

        int fieldIndex = 0;
        for (auto &fieldEntry : module->getCurrentStructure()->getFields()) {
            Field* field = fieldEntry.second;
            llvm::Value* allocation = module->getBuilder()->CreateStructGEP(currentField->getType()->getLlvmType(),
                                                                            currentField->getAllocation(), fieldIndex,
                                                                            field->getName());
            field->setAllocation(allocation);
            module->getCurrentStructure()->getScope()->fields[field->getName()] = field;
            fieldIndex++;
        }

        if (constructor == nullptr) {
            throw "A constructor with the specified signature could not be found.";
        } else {
            module->getBuilder()->CreateCall(constructor->getLlvmFunctionType(),
                                             constructor->getLlvmFunction(),
                                             values);
        }
        return nullptr;
    }

    llvm::Value* NewExpression::generateCode(CajetaModule* module) {
        CajetaStructure* structure = module->getCurrentStructure();
        Field* field = module->getCurrentField();

        const llvm::DataLayout& dataLayout = module->getLlvmModule()->getDataLayout();

        vector<llvm::Value*> mallocArgs;
        llvm::Value* value = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*module->getLlvmContext()),
                                                    dataLayout.getTypeAllocSize(structure->getLlvmType()));
        mallocArgs.push_back(value);
        llvm::BasicBlock* block = module->getBuilder()->GetInsertBlock();
        llvm::Constant* allocSize = llvm::ConstantExpr::getSizeOf(structure->getLlvmType());
        MemoryManager* memoryAllocator = MemoryManager::getInstance(module);
        llvm::Instruction* mallocInst = memoryAllocator->createMallocInstruction(field->getName(), allocSize, block);
        field->setAllocation(mallocInst);
        this->creatorRest->generateCode(module);
        return mallocInst;
    }

    CajetaType* NewExpression::toType(CajetaModule* module) {
        return module->getCurrentStructure();
    }
}

