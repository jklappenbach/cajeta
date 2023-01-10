
// Generated of /Users/julian/code/cpp/cajeta/antlr4/CajetaParser.g4 by ANTLR 4.9.3

#pragma once


#include "../../../build/antlr4_runtime/src/antlr4_runtime/runtime/Cpp/runtime/src/antlr4-runtime.h"
#include "CajetaParserVisitor.h"
#include "../type/CajetaStructure.h"
#include "../type/CajetaClass.h"
#include "../../../build/antlr4_runtime/src/antlr4_runtime/runtime/Cpp/runtime/src/support/Any.h"
#include "../asn/Block.h"
#include "../asn/expression/Expression.h"
#include "../asn/LocalVariableDeclaration.h"
#include "CajetaModule.h"
#include "../asn/ClassBodyDeclaration.h"


namespace cajeta {

    /**
     * This class provides an empty implementation of CajetaLlvmVisitor, which can be
     * extended to getOrCreate a visitor which only needs to handle a subset of the available methods.
     */
    class CajetaLlvmVisitor : public CajetaParserVisitor {
    private:
        CajetaModulePtr module;
    public:
        CajetaLlvmVisitor(CajetaModulePtr module) {
            this->module = module;
        }

        CajetaModulePtr getCajetaModule() const {
            return module;
        }

        virtual std::any visitCompilationUnit(CajetaParser::CompilationUnitContext* ctx) override {
            module->onPackageDeclaration(ctx->packageDeclaration());
            for (auto& importDeclarationContext: ctx->importDeclaration()) {
                module->onImportDeclaration(importDeclarationContext);
            }
            for (auto& typeDeclarationContext: ctx->typeDeclaration()) {
                module->onStructureDeclaration(visitChildren(typeDeclarationContext));
            }
            return std::any(nullptr);
        }

        virtual std::any visitPackageDeclaration(CajetaParser::PackageDeclarationContext* ctx) override {
            module->onPackageDeclaration(ctx);
            return visitChildren(ctx);
        }

        virtual std::any visitImportDeclaration(CajetaParser::ImportDeclarationContext* ctx) override {
            module->onImportDeclaration(ctx);
            return visitChildren(ctx);
        }

        virtual std::any visitTypeDeclaration(CajetaParser::TypeDeclarationContext* ctx) override {
            cout << "What is going on with visitTypeDeclaration.";
            return visitChildren(ctx);
        }

        virtual std::any visitModifier(CajetaParser::ModifierContext* ctx) override {
            return Modifiable::toModifier(ctx->getText());
        }

        virtual std::any
        visitClassOrInterfaceModifier(CajetaParser::ClassOrInterfaceModifierContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitVariableModifier(CajetaParser::VariableModifierContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitClassDeclaration(CajetaParser::ClassDeclarationContext* ctx) override {
            string packageAdj;
            string name = ctx->identifier()->getText();
            for (auto& structure: module->getStructureStack()) {
                packageAdj.append(".");
                packageAdj.append(structure->getQName()->getTypeName());
            }
            QualifiedNamePtr qName = QualifiedName::getOrInsert(name, module->getQName()->getPackageName() + packageAdj);
            list<QualifiedNamePtr> qExtended;
            list<QualifiedNamePtr> qImplemented;
            for (auto& ctxTypeList : ctx->typeList()) {
                string str = ctxTypeList->getText();
                for (auto& ctxTypeType : ctxTypeList->typeType()) {
                    qExtended.push_back(QualifiedName::fromContext(ctxTypeType->classOrInterfaceType()));
                }
            }
            CajetaStructurePtr structure = make_shared<CajetaClass>(module, qName, qExtended, qImplemented);
            module->getStructureStack().push_back(structure);
            structure->setClassBody(std::any_cast<ClassBodyDeclarationPtr>(visitChildren(ctx)));
            structure->generatePrototype();
            module->getStructureStack().pop_back();
            return structure;
        }

        virtual std::any visitTypeParameters(CajetaParser::TypeParametersContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitTypeParameter(CajetaParser::TypeParameterContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitTypeBound(CajetaParser::TypeBoundContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitEnumDeclaration(CajetaParser::EnumDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitEnumConstants(CajetaParser::EnumConstantsContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitEnumConstant(CajetaParser::EnumConstantContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitEnumBodyDeclarations(CajetaParser::EnumBodyDeclarationsContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitInterfaceDeclaration(CajetaParser::InterfaceDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitClassBody(CajetaParser::ClassBodyContext* ctx) override {
            ClassBodyDeclarationPtr classBody = make_shared<ClassBodyDeclaration>(ctx->getStart());
            for (auto& classBodyDeclarationCtx: ctx->classBodyDeclaration()) {
                MemberDeclarationPtr memberDeclaration = std::any_cast<MemberDeclarationPtr>(visitClassBodyDeclaration(classBodyDeclarationCtx));
                classBody->getDeclarations().push_back(memberDeclaration);
            }
            return classBody;
        }

        virtual std::any visitInterfaceBody(CajetaParser::InterfaceBodyContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitClassBodyDeclaration(CajetaParser::ClassBodyDeclarationContext* ctx) override {
            MemberDeclarationPtr memberDeclaration = any_cast<MemberDeclarationPtr>(visitMemberDeclaration(
                ctx->memberDeclaration()));
            for (auto& modifierContext: ctx->modifier()) {
                memberDeclaration->onModifier(any_cast<Modifier>(visitModifier(modifierContext)));
            }
            return memberDeclaration;
        }

        virtual std::any visitMemberDeclaration(CajetaParser::MemberDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitOperatorOverloadDeclaration(CajetaParser::OperatorOverloadDeclarationContext *ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitGenericOperatorOverloadDeclaration(CajetaParser::GenericOperatorOverloadDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitMethodDeclaration(CajetaParser::MethodDeclarationContext* ctx) override {
            string name = ctx->identifier()->getText();
            vector<FormalParameterPtr> formalParameters;
            if (ctx->formalParameters()->formalParameterList()) {
                for (auto& formalParameterContext: ctx->formalParameters()->formalParameterList()->formalParameter()) {
                    formalParameters.push_back(FormalParameter::fromContext(formalParameterContext, module));
                }
            }
            CajetaTypePtr returnType = CajetaType::fromContext(ctx->typeTypeOrVoid(), module);
            BlockPtr block = any_cast<BlockPtr>(visitMethodBody(ctx->methodBody()));
            MethodPtr method = Method::create(
                this->module,
                name,
                returnType,
                formalParameters,
                block,
                module->getStructureStack().front());
            return static_pointer_cast<MemberDeclaration>(make_shared<MethodDeclaration>(method, ctx->getStart()));
        }

        /**
         * For prototype discovery, we want to only parse up to the point where we have structure and method
         * prototype definitions.  This will allow all CU prototypes to be defined before method definitions are
         * processed.
         *
         * @param ctx The MethodBodyContext
         * @return An Any structure, containing the block of the method.
         */
        virtual std::any visitMethodBody(CajetaParser::MethodBodyContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitTypeTypeOrVoid(CajetaParser::TypeTypeOrVoidContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any
        visitGenericMethodDeclaration(CajetaParser::GenericMethodDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any
        visitGenericConstructorDeclaration(CajetaParser::GenericConstructorDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitConstructorDeclaration(CajetaParser::ConstructorDeclarationContext* ctx) override {
            string name = ctx->identifier()->getText();
            vector<FormalParameterPtr> formalParameters;
            if (ctx->formalParameters()->formalParameterList()) {
                for (auto& formalParameterContext: ctx->formalParameters()->formalParameterList()->formalParameter()) {
                    formalParameters.push_back(FormalParameter::fromContext(formalParameterContext, module));
                }
            }
            BlockPtr block = any_cast<BlockPtr>(visitBlock(ctx->constructorBody));
            MethodPtr method = Method::create(module, name,
                CajetaType::of("void"),
                formalParameters,
                block,
                module->getStructureStack().back());
            return static_pointer_cast<MemberDeclaration>(make_shared<MethodDeclaration>(method, ctx->getStart()));
        }

        // TODO: Scrap this and replace with a
        virtual std::any visitFieldDeclaration(CajetaParser::FieldDeclarationContext* ctx) override {
            CajetaTypePtr type;
            list<VariableDeclaratorPtr> variableDeclarators;
            antlr4::Token* token;
            return static_pointer_cast<MemberDeclaration>(
                make_shared<FieldDeclaration>(
                any_cast<CajetaTypePtr>(visitTypeType(ctx->typeType())),
                any_cast<list<VariableDeclaratorPtr>>(visitVariableDeclarators(ctx->variableDeclarators())),
                ctx->getStart()));
        }

        virtual std::any
        visitInterfaceBodyDeclaration(CajetaParser::InterfaceBodyDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any
        visitInterfaceMemberDeclaration(CajetaParser::InterfaceMemberDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitConstDeclaration(CajetaParser::ConstDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitConstantDeclarator(CajetaParser::ConstantDeclaratorContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any
        visitInterfaceMethodDeclaration(CajetaParser::InterfaceMethodDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitInterfaceMethodModifier(CajetaParser::InterfaceMethodModifierContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any
        visitGenericInterfaceMethodDeclaration(CajetaParser::GenericInterfaceMethodDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any
        visitInterfaceCommonBodyDeclaration(CajetaParser::InterfaceCommonBodyDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitVariableDeclarators(CajetaParser::VariableDeclaratorsContext* ctx) override {
            list<VariableDeclaratorPtr> variableDeclarators;
            for (auto& variableDeclaratorContext: ctx->variableDeclarator()) {
                variableDeclarators.push_back(any_cast<VariableDeclaratorPtr>(visitVariableDeclarator(variableDeclaratorContext)));
            }
            return variableDeclarators;
        }

        virtual std::any visitVariableDeclarator(CajetaParser::VariableDeclaratorContext* ctx) override {
            InitializerPtr initializer = nullptr;

            if (ctx->variableInitializer() != nullptr) {
                initializer = any_cast<InitializerPtr>(visitVariableInitializer(ctx->variableInitializer()));
            }

            return make_shared<VariableDeclarator>(
                ctx->variableDeclaratorId()->identifier()->getText(),
                ctx->variableDeclaratorId()->LBRACK().size(),
                ctx->REFERENCE() != nullptr,
                initializer,
                ctx->getStart());
        }

        virtual std::any visitVariableDeclaratorId(CajetaParser::VariableDeclaratorIdContext* ctx) override {
            return visitChildren(ctx);
        }

        // TODO: Need to update this to accept parameter labels!
        virtual std::any visitVariableInitializer(CajetaParser::VariableInitializerContext* ctx) override {
            if (ctx->arrayInitializer()) {
                return visitArrayInitializer(ctx->arrayInitializer());
            }
            return static_pointer_cast<Initializer>(make_shared<VariableInitializer>(any_cast<ExpressionPtr>(visitExpression(ctx->expression())),
                ctx->getStart()));
        }

        virtual std::any visitArrayInitializer(CajetaParser::ArrayInitializerContext* ctx) override {
            list<InitializerPtr> initializers;
            for (auto& variableInitializerContext: ctx->variableInitializer()) {
                initializers.push_back(any_cast<InitializerPtr>(visitVariableInitializer(variableInitializerContext)));
            }
            return make_shared<ArrayInitializer>(initializers, ctx->getStart());
        }

        virtual std::any visitClassOrInterfaceType(CajetaParser::ClassOrInterfaceTypeContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitTypeArgument(CajetaParser::TypeArgumentContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitQualifiedNameList(CajetaParser::QualifiedNameListContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitFormalParameters(CajetaParser::FormalParametersContext* ctx) override {
            return visitFormalParameterList(ctx->formalParameterList());
        }

        virtual std::any visitReceiverParameter(CajetaParser::ReceiverParameterContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitFormalParameterList(CajetaParser::FormalParameterListContext* ctx) override {
            list<FormalParameterPtr> formalParameters;
            if (ctx) {
                for (auto& formalParameterContext: ctx->formalParameter()) {
                    formalParameters.push_back(FormalParameter::fromContext(formalParameterContext, module));
                }
            }

            return formalParameters;
        }

        virtual std::any visitFormalParameter(CajetaParser::FormalParameterContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitLastFormalParameter(CajetaParser::LastFormalParameterContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitLambdaLVTIList(CajetaParser::LambdaLVTIListContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitLambdaLVTIParameter(CajetaParser::LambdaLVTIParameterContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitQualifiedName(CajetaParser::QualifiedNameContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitLiteral(CajetaParser::LiteralContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitIntegerLiteral(CajetaParser::IntegerLiteralContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitFloatLiteral(CajetaParser::FloatLiteralContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any
        visitAltAnnotationQualifiedName(CajetaParser::AltAnnotationQualifiedNameContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitAnnotation(CajetaParser::AnnotationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitElementValuePairs(CajetaParser::ElementValuePairsContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitElementValuePair(CajetaParser::ElementValuePairContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitElementValue(CajetaParser::ElementValueContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any
        visitElementValueArrayInitializer(CajetaParser::ElementValueArrayInitializerContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any
        visitAnnotationTypeDeclaration(CajetaParser::AnnotationTypeDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitAnnotationTypeBody(CajetaParser::AnnotationTypeBodyContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any
        visitAnnotationTypeElementDeclaration(CajetaParser::AnnotationTypeElementDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any
        visitAnnotationTypeElementRest(CajetaParser::AnnotationTypeElementRestContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any
        visitAnnotationMethodOrConstantRest(CajetaParser::AnnotationMethodOrConstantRestContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitAnnotationMethodRest(CajetaParser::AnnotationMethodRestContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitAnnotationConstantRest(CajetaParser::AnnotationConstantRestContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitDefaultValue(CajetaParser::DefaultValueContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitModuleDeclaration(CajetaParser::ModuleDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitModuleBody(CajetaParser::ModuleBodyContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitModuleDirective(CajetaParser::ModuleDirectiveContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitRequiresModifier(CajetaParser::RequiresModifierContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitRecordDeclaration(CajetaParser::RecordDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitRecordHeader(CajetaParser::RecordHeaderContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitRecordComponentList(CajetaParser::RecordComponentListContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitRecordComponent(CajetaParser::RecordComponentContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitRecordBody(CajetaParser::RecordBodyContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitBlock(CajetaParser::BlockContext* ctx) override {
            BlockPtr block = make_shared<Block>(ctx->getStart());
            for (auto& blockStatementContext: ctx->blockStatement()) {
                block->addChild(any_cast<BlockStatementPtr>(visitBlockStatement(blockStatementContext)));
            }
            return block;
        }

        virtual std::any visitBlockStatement(CajetaParser::BlockStatementContext* ctx) override {
            if (ctx->localVariableDeclaration()) {
                return visitLocalVariableDeclaration(ctx->localVariableDeclaration());
            } else if (ctx->statement()) {
                return visitStatement(ctx->statement());
            } else if (ctx->localTypeDeclaration()) {
                return visitLocalTypeDeclaration(ctx->localTypeDeclaration());
            }
            return visitChildren(ctx);
        }

        virtual std::any
        visitLocalVariableDeclaration(CajetaParser::LocalVariableDeclarationContext* ctx) override {
            set<Modifier> modifiers;
            for (auto& variableModifierContext: ctx->variableModifier()) {
                modifiers.insert(Modifiable::toModifier(variableModifierContext->getText()));
            }
            return static_pointer_cast<BlockStatement>(make_shared<LocalVariableDeclaration>(
                modifiers,
                CajetaType::fromContext(ctx->typeType(), module),
                any_cast<list<VariableDeclaratorPtr>>(visitVariableDeclarators(ctx->variableDeclarators())),
                ctx->getStart()));
        }

        virtual std::any visitIdentifier(CajetaParser::IdentifierContext* ctx) override {
            return ctx->getText();
        }

        virtual std::any visitLocalTypeDeclaration(CajetaParser::LocalTypeDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitStatement(CajetaParser::StatementContext* ctx) override {
            return static_pointer_cast<BlockStatement>(Statement::fromContext(ctx));
        }

        virtual std::any visitCatchClause(CajetaParser::CatchClauseContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitCatchType(CajetaParser::CatchTypeContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitFinallyBlock(CajetaParser::FinallyBlockContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitResourceSpecification(CajetaParser::ResourceSpecificationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitResources(CajetaParser::ResourcesContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitResource(CajetaParser::ResourceContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any
        visitSwitchBlockStatementGroup(CajetaParser::SwitchBlockStatementGroupContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitSwitchLabel(CajetaParser::SwitchLabelContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitForControl(CajetaParser::ForControlContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitForInit(CajetaParser::ForInitContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitEnhancedForControl(CajetaParser::EnhancedForControlContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitLoopVariable(CajetaParser::LoopVariableContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitLoopIterator(CajetaParser::LoopIteratorContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitParExpression(CajetaParser::ParExpressionContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitExpressionList(CajetaParser::ExpressionListContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitParameterList(CajetaParser::ParameterListContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitParameterEntry(CajetaParser::ParameterEntryContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitParameterLabel(CajetaParser::ParameterLabelContext* ctx) override {
            return visitChildren(ctx);
        };

        virtual std::any visitMethodCall(CajetaParser::MethodCallContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitExpression(CajetaParser::ExpressionContext* ctx) override {
            ExpressionPtr expression = Expression::fromContext(ctx);
            for (auto& expressionContext: ctx->expression()) {
                std::any any = visitChildren(expressionContext->parent);
                expression->addChild(std::any_cast<ExpressionPtr>(any));
            }
            return expression;
        }

        virtual std::any visitPattern(CajetaParser::PatternContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitLambdaExpression(CajetaParser::LambdaExpressionContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitLambdaParameters(CajetaParser::LambdaParametersContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitLambdaBody(CajetaParser::LambdaBodyContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitPrimary(CajetaParser::PrimaryContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitSwitchExpression(CajetaParser::SwitchExpressionContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitSwitchLabeledRule(CajetaParser::SwitchLabeledRuleContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitGuardedPattern(CajetaParser::GuardedPatternContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitSwitchRuleOutcome(CajetaParser::SwitchRuleOutcomeContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitClassType(CajetaParser::ClassTypeContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitCreator(CajetaParser::CreatorContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitCreatedName(CajetaParser::CreatedNameContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitInnerCreator(CajetaParser::InnerCreatorContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitArrayCreatorRest(CajetaParser::ArrayCreatorRestContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitClassCreatorRest(CajetaParser::ClassCreatorRestContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any
        visitExplicitGenericInvocation(CajetaParser::ExplicitGenericInvocationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitTypeArgumentsOrDiamond(CajetaParser::TypeArgumentsOrDiamondContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any
        visitNonWildcardTypeArgumentsOrDiamond(CajetaParser::NonWildcardTypeArgumentsOrDiamondContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any
        visitNonWildcardTypeArguments(CajetaParser::NonWildcardTypeArgumentsContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitTypeList(CajetaParser::TypeListContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitTypeType(CajetaParser::TypeTypeContext* ctx) override {
            return CajetaType::fromContext(ctx, module);
        }

        virtual std::any visitPrimitiveType(CajetaParser::PrimitiveTypeContext* ctx) override {
            return CajetaType::fromContext(ctx, module);
        }

        virtual std::any visitTypeArguments(CajetaParser::TypeArgumentsContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitSuperSuffix(CajetaParser::SuperSuffixContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any
        visitExplicitGenericInvocationSuffix(CajetaParser::ExplicitGenericInvocationSuffixContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitArguments(CajetaParser::ArgumentsContext* ctx) override {
            return visitChildren(ctx);
        }
    };
}  // namespace cajeta
