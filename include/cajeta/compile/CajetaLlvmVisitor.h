
// Generated of /Users/julian/code/cpp/cajeta/antlr4/CajetaParser.g4 by ANTLR 4.9.3

#pragma once


#include "antlr4-runtime.h"
#include "CajetaParserVisitor.h"
#include "cajeta/type/CajetaStructure.h"
#include "cajeta/type/CajetaClass.h"
#include <support/Any.h>
#include <cajeta/asn/Block.h>
#include <cajeta/asn/Expression.h>
#include <cajeta/asn/LocalVariableDeclaration.h>
#include <cajeta/compile/CajetaModule.h>
#include <cajeta/asn/ClassBodyDeclaration.h>


namespace cajeta {

    /**
     * This class provides an empty implementation of CajetaLlvmVisitor, which can be
     * extended to getOrInsert a visitor which only needs to handle a subset of the available methods.
     */
    class CajetaLlvmVisitor : public CajetaParserVisitor {
    private:
        CajetaModule* module;
    public:
        CajetaLlvmVisitor(CajetaModule* module) {
            this->module = module;
        }

        CajetaModule* getCajetaModule() const {
            return module;
        }

        virtual antlrcpp::Any visitCompilationUnit(CajetaParser::CompilationUnitContext* ctx) override {
            module->onPackageDeclaration(ctx->packageDeclaration());
            for (auto &importDeclarationContext : ctx->importDeclaration()) {
                module->onImportDeclaration(importDeclarationContext);
            }
            for (auto &typeDeclarationContext : ctx->typeDeclaration()) {
                module->onStructureDeclaration(visitChildren(typeDeclarationContext));
            }
            return antlrcpp::Any(nullptr);
        }

        virtual antlrcpp::Any visitPackageDeclaration(CajetaParser::PackageDeclarationContext* ctx) override {
            module->onPackageDeclaration(ctx);
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitImportDeclaration(CajetaParser::ImportDeclarationContext* ctx) override {
            module->onImportDeclaration(ctx);
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitTypeDeclaration(CajetaParser::TypeDeclarationContext* ctx) override {
            cout << "What is going on with visitTypeDeclaration.";
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitModifier(CajetaParser::ModifierContext* ctx) override {
            return Modifiable::toModifier(ctx->getText());
        }

        virtual antlrcpp::Any visitClassOrInterfaceModifier(CajetaParser::ClassOrInterfaceModifierContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitVariableModifier(CajetaParser::VariableModifierContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitClassDeclaration(CajetaParser::ClassDeclarationContext* ctx) override {
            string packageAdj;
            string name = ctx->identifier()->getText();
            for (auto &structure : module->getStructureStack()) {
                packageAdj.append(".");
                packageAdj.append(structure->getQName()->getTypeName());
            }
            QualifiedName* qName = QualifiedName::getOrInsert(name, module->getQName()->getPackageName()
                                                                    + packageAdj);
            CajetaStructure* structure;
            structure = new CajetaClass(module, qName);
            module->getStructureStack().push_back(structure);
            structure->setClassBody(visitChildren(ctx).as<ClassBodyDeclaration*>());
            structure->generateSignature(module);
            module->getStructureStack().pop_back();
            return structure;
        }

        virtual antlrcpp::Any visitTypeParameters(CajetaParser::TypeParametersContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitTypeParameter(CajetaParser::TypeParameterContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitTypeBound(CajetaParser::TypeBoundContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitEnumDeclaration(CajetaParser::EnumDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitEnumConstants(CajetaParser::EnumConstantsContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitEnumConstant(CajetaParser::EnumConstantContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitEnumBodyDeclarations(CajetaParser::EnumBodyDeclarationsContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitInterfaceDeclaration(CajetaParser::InterfaceDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitClassBody(CajetaParser::ClassBodyContext* ctx) override {
            ClassBodyDeclaration* classBody = new ClassBodyDeclaration(ctx->getStart());
            for (auto &classBodyDeclarationCtx : ctx->classBodyDeclaration()) {
                classBody->getDeclarations().push_back(visitClassBodyDeclaration(classBodyDeclarationCtx)
                                                               .as<MemberDeclaration*>());
            }
            return classBody;
        }

        virtual antlrcpp::Any visitInterfaceBody(CajetaParser::InterfaceBodyContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitClassBodyDeclaration(CajetaParser::ClassBodyDeclarationContext* ctx) override {
            MemberDeclaration* memberDeclaration = visitMemberDeclaration(ctx->memberDeclaration()).as<MemberDeclaration*>();
            for (auto &modifierContext : ctx->modifier()) {
                memberDeclaration->onModifier(visitModifier(modifierContext).as<Modifier>());
            }
            return memberDeclaration;
        }

        virtual antlrcpp::Any visitMemberDeclaration(CajetaParser::MemberDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitMethodDeclaration(CajetaParser::MethodDeclarationContext* ctx) override {
            string name = ctx->identifier()->getText();
            vector<FormalParameter*> formalParameters;
            if (ctx->formalParameters()->formalParameterList()) {
                for (auto &formalParameterContext : ctx->formalParameters()->formalParameterList()->formalParameter()) {
                    formalParameters.push_back(FormalParameter::fromContext(formalParameterContext));
                }
            }
            CajetaType* returnType = CajetaType::fromContext(ctx->typeTypeOrVoid());
            Block* block = visitMethodBody(ctx->methodBody()).as<Block*>();
            Method* method = new Method(name,
                                        returnType,
                                        formalParameters,
                                        block,
                                        module->getStructureStack().front());
            return (MemberDeclaration*) new MethodDeclaration(method, ctx->getStart());
        }

        /**
         * For prototype discovery, we want to only parse up to the point where we have structure and method
         * prototype definitions.  This will allow all CU prototypes to be defined before method definitions are
         * processed.
         *
         * @param ctx The MethodBodyContext
         * @return An Any structure, containing the block of the method.
         */
        virtual antlrcpp::Any visitMethodBody(CajetaParser::MethodBodyContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitTypeTypeOrVoid(CajetaParser::TypeTypeOrVoidContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitGenericMethodDeclaration(CajetaParser::GenericMethodDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitGenericConstructorDeclaration(CajetaParser::GenericConstructorDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitConstructorDeclaration(CajetaParser::ConstructorDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        // TODO: Scrap this and replace with a
        virtual antlrcpp::Any visitFieldDeclaration(CajetaParser::FieldDeclarationContext* ctx) override {
            return (MemberDeclaration*) new FieldDeclaration(
                visitTypeType(ctx->typeType()).as<CajetaType*>(),
                visitVariableDeclarators(ctx->variableDeclarators()).as<list<VariableDeclarator*>>(), ctx->getStart());
        }

        virtual antlrcpp::Any visitInterfaceBodyDeclaration(CajetaParser::InterfaceBodyDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitInterfaceMemberDeclaration(CajetaParser::InterfaceMemberDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitConstDeclaration(CajetaParser::ConstDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitConstantDeclarator(CajetaParser::ConstantDeclaratorContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitInterfaceMethodDeclaration(CajetaParser::InterfaceMethodDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitInterfaceMethodModifier(CajetaParser::InterfaceMethodModifierContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitGenericInterfaceMethodDeclaration(CajetaParser::GenericInterfaceMethodDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitInterfaceCommonBodyDeclaration(CajetaParser::InterfaceCommonBodyDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitVariableDeclarators(CajetaParser::VariableDeclaratorsContext* ctx) override {
            list<VariableDeclarator*> variableDeclarators;
            for (auto &variableDeclaratorContext : ctx->variableDeclarator()) {
                variableDeclarators.push_back(visitVariableDeclarator(variableDeclaratorContext)
                        .as<VariableDeclarator*>());
            }
            return variableDeclarators;
        }

        virtual antlrcpp::Any visitVariableDeclarator(CajetaParser::VariableDeclaratorContext* ctx) override {
            Initializer* initializer = nullptr;

            if (ctx->variableInitializer() != nullptr) {
                initializer = visitVariableInitializer(ctx->variableInitializer()).as<Initializer*>();
            }

            return new VariableDeclarator(
                    ctx->variableDeclaratorId()->identifier()->getText(),
                    ctx->variableDeclaratorId()->LBRACK().size(),
                    ctx->REFERENCE() != nullptr,
                    initializer,
                    ctx->getStart());
        }

        virtual antlrcpp::Any visitVariableDeclaratorId(CajetaParser::VariableDeclaratorIdContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitVariableInitializer(CajetaParser::VariableInitializerContext* ctx) override {
            if (ctx->arrayInitializer()) {
                return visitArrayInitializer(ctx->arrayInitializer());
            }
            return (Initializer*) new VariableInitializer(visitExpression(ctx->expression()).as<Expression*>(),
                    ctx->getStart());
        }

        virtual antlrcpp::Any visitArrayInitializer(CajetaParser::ArrayInitializerContext* ctx) override {
            list<VariableInitializer*> initializers;
            for (auto &variableInitializerContext : ctx->variableInitializer()) {
                initializers.push_back(visitVariableInitializer(variableInitializerContext)
                        .as<VariableInitializer*>());
            }
            return (Initializer*) new ArrayInitializer(initializers, ctx->getStart());
        }

        virtual antlrcpp::Any visitClassOrInterfaceType(CajetaParser::ClassOrInterfaceTypeContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitTypeArgument(CajetaParser::TypeArgumentContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitQualifiedNameList(CajetaParser::QualifiedNameListContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitFormalParameters(CajetaParser::FormalParametersContext* ctx) override {
            return visitChildren(ctx->formalParameterList()).as<list<FormalParameter*>>();
        }

        virtual antlrcpp::Any visitReceiverParameter(CajetaParser::ReceiverParameterContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitFormalParameterList(CajetaParser::FormalParameterListContext* ctx) override {
            list<FormalParameter*> formalParameters;
            for (auto &formalParameterContext : ctx->formalParameter()) {
                formalParameters.push_back(FormalParameter::fromContext(formalParameterContext));
            }

            return formalParameters;
        }

        virtual antlrcpp::Any visitFormalParameter(CajetaParser::FormalParameterContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitLastFormalParameter(CajetaParser::LastFormalParameterContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitLambdaLVTIList(CajetaParser::LambdaLVTIListContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitLambdaLVTIParameter(CajetaParser::LambdaLVTIParameterContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitQualifiedName(CajetaParser::QualifiedNameContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitLiteral(CajetaParser::LiteralContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitIntegerLiteral(CajetaParser::IntegerLiteralContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitFloatLiteral(CajetaParser::FloatLiteralContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitAltAnnotationQualifiedName(CajetaParser::AltAnnotationQualifiedNameContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitAnnotation(CajetaParser::AnnotationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitElementValuePairs(CajetaParser::ElementValuePairsContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitElementValuePair(CajetaParser::ElementValuePairContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitElementValue(CajetaParser::ElementValueContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitElementValueArrayInitializer(CajetaParser::ElementValueArrayInitializerContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitAnnotationTypeDeclaration(CajetaParser::AnnotationTypeDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitAnnotationTypeBody(CajetaParser::AnnotationTypeBodyContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitAnnotationTypeElementDeclaration(CajetaParser::AnnotationTypeElementDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitAnnotationTypeElementRest(CajetaParser::AnnotationTypeElementRestContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitAnnotationMethodOrConstantRest(CajetaParser::AnnotationMethodOrConstantRestContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitAnnotationMethodRest(CajetaParser::AnnotationMethodRestContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitAnnotationConstantRest(CajetaParser::AnnotationConstantRestContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitDefaultValue(CajetaParser::DefaultValueContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitModuleDeclaration(CajetaParser::ModuleDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitModuleBody(CajetaParser::ModuleBodyContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitModuleDirective(CajetaParser::ModuleDirectiveContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitRequiresModifier(CajetaParser::RequiresModifierContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitRecordDeclaration(CajetaParser::RecordDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitRecordHeader(CajetaParser::RecordHeaderContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitRecordComponentList(CajetaParser::RecordComponentListContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitRecordComponent(CajetaParser::RecordComponentContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitRecordBody(CajetaParser::RecordBodyContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitBlock(CajetaParser::BlockContext* ctx) override {
            Block* block = new Block(ctx->getStart());
            for (auto &blockStatementContext : ctx->blockStatement()) {
                block->addChild(visitBlockStatement(blockStatementContext).as<BlockStatement*>());
            }
            return block;
        }

        virtual antlrcpp::Any visitBlockStatement(CajetaParser::BlockStatementContext* ctx) override {
            if (ctx->localVariableDeclaration()) {
                return visitLocalVariableDeclaration(ctx->localVariableDeclaration());
            } else if (ctx->statement()) {
                return visitStatement(ctx->statement());
            } else if (ctx->localTypeDeclaration()) {
                return visitLocalTypeDeclaration(ctx->localTypeDeclaration());
            }
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitLocalVariableDeclaration(CajetaParser::LocalVariableDeclarationContext* ctx) override {
            set<Modifier> modifiers;
            for (auto &variableModifierContext : ctx->variableModifier()) {
                modifiers.insert(Modifiable::toModifier(variableModifierContext->getText()));
            }
            return (BlockStatement*) new LocalVariableDeclaration(
                    modifiers,
                    CajetaType::fromContext(ctx->typeType()),
                    visitVariableDeclarators(ctx->variableDeclarators()).as<list<VariableDeclarator*>>(),
                    ctx->getStart());
        }

        virtual antlrcpp::Any visitIdentifier(CajetaParser::IdentifierContext* ctx) override {
            return ctx->getText();
        }

        virtual antlrcpp::Any visitLocalTypeDeclaration(CajetaParser::LocalTypeDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitStatement(CajetaParser::StatementContext* ctx) override {
            return (BlockStatement*) Statement::fromContext(ctx);
        }

        virtual antlrcpp::Any visitCatchClause(CajetaParser::CatchClauseContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitCatchType(CajetaParser::CatchTypeContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitFinallyBlock(CajetaParser::FinallyBlockContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitResourceSpecification(CajetaParser::ResourceSpecificationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitResources(CajetaParser::ResourcesContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitResource(CajetaParser::ResourceContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitSwitchBlockStatementGroup(CajetaParser::SwitchBlockStatementGroupContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitSwitchLabel(CajetaParser::SwitchLabelContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitForControl(CajetaParser::ForControlContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitForInit(CajetaParser::ForInitContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitEnhancedForControl(CajetaParser::EnhancedForControlContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitLoopVariable(CajetaParser::LoopVariableContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitLoopIterator(CajetaParser::LoopIteratorContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitParExpression(CajetaParser::ParExpressionContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitExpressionList(CajetaParser::ExpressionListContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitParameterList(CajetaParser::ParameterListContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitParameterLabel(CajetaParser::ParameterLabelContext* ctx) override {
            return visitChildren(ctx);
        };

        virtual antlrcpp::Any visitMethodCall(CajetaParser::MethodCallContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitExpression(CajetaParser::ExpressionContext* ctx) override {
            Expression* expression = Expression::fromContext(ctx);
            for (auto &expressionContext : ctx->expression()) {
                antlrcpp::Any any = visitChildren(expressionContext->parent);
                expression->addChild(any.as<Expression*>());
            }
            return expression;
        }

        virtual antlrcpp::Any visitPattern(CajetaParser::PatternContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitLambdaExpression(CajetaParser::LambdaExpressionContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitLambdaParameters(CajetaParser::LambdaParametersContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitLambdaBody(CajetaParser::LambdaBodyContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitPrimary(CajetaParser::PrimaryContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitSwitchExpression(CajetaParser::SwitchExpressionContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitSwitchLabeledRule(CajetaParser::SwitchLabeledRuleContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitGuardedPattern(CajetaParser::GuardedPatternContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitSwitchRuleOutcome(CajetaParser::SwitchRuleOutcomeContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitClassType(CajetaParser::ClassTypeContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitCreator(CajetaParser::CreatorContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitCreatedName(CajetaParser::CreatedNameContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitInnerCreator(CajetaParser::InnerCreatorContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitArrayCreatorRest(CajetaParser::ArrayCreatorRestContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitClassCreatorRest(CajetaParser::ClassCreatorRestContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitExplicitGenericInvocation(CajetaParser::ExplicitGenericInvocationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitTypeArgumentsOrDiamond(CajetaParser::TypeArgumentsOrDiamondContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitNonWildcardTypeArgumentsOrDiamond(CajetaParser::NonWildcardTypeArgumentsOrDiamondContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitNonWildcardTypeArguments(CajetaParser::NonWildcardTypeArgumentsContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitTypeList(CajetaParser::TypeListContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitTypeType(CajetaParser::TypeTypeContext* ctx) override {
            return CajetaType::fromContext(ctx);
        }

        virtual antlrcpp::Any visitPrimitiveType(CajetaParser::PrimitiveTypeContext* ctx) override {
            return CajetaType::fromContext(ctx);
        }

        virtual antlrcpp::Any visitTypeArguments(CajetaParser::TypeArgumentsContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitSuperSuffix(CajetaParser::SuperSuffixContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitExplicitGenericInvocationSuffix(CajetaParser::ExplicitGenericInvocationSuffixContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual antlrcpp::Any visitArguments(CajetaParser::ArgumentsContext* ctx) override {
            return visitChildren(ctx);
        }
    };
}  // namespace cajeta
