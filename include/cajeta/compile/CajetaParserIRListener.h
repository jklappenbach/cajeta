#pragma once

#include <llvm/Target/TargetOptions.h>
#include "CajetaParser.h"
#include <llvm/Bitcode/BitcodeWriter.h>
#include "CajetaParserBaseListener.h"
#include <llvm/ADT/StringRef.h>
#include "cajeta/ast/Statement.h"
#include "cajeta/type/Generics.h"
#include "cajeta/ast/Scope.h"
#include "cajeta/type/Method.h"
#include "cajeta/ast/Block.h"
#include "cajeta/type/Modifiable.h"
#include "cajeta/type/Annotatable.h"
#include "cajeta/module/CompilationUnit.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/Interface.h"
#include "cajeta/type/CajetaEnum.h"
#include "cajeta/ast/FormalParameter.h"
#include "cajeta/module/CompilationUnit.h"
#include "cajeta/compile/ParseContext.h"


using namespace std;

namespace cajeta {
    class CajetaParserIRListener : public CajetaParserBaseListener, public ParseContext {
    private:
        set<QualifiedName*> curAnnotations;
        set<Modifier> modifiers;
        CajetaStructure* curStructure;
        Method* curMethod;

        // Parsing state

    public:
        CajetaParserIRListener(CompilationUnit* compilationUnit,
                              llvm::LLVMContext* llvmContext,
                              string targetTriple,
                              llvm::TargetMachine* targetMachine) :
                              ParseContext(compilationUnit, llvmContext, targetTriple, targetMachine) {
//            CajetaLexer
//            legacy::PassManager pass;
//            auto FileType = llvm::CGFT_ObjectFile;
//
//            string error;
//            if (targetMachine->addPassesToEmitFile(pass, dest, nullptr, FileType)) {
//                error = "TargetMachine can't emit a file of this type";
//                return;
//            }
//
//            pass.run(*module);
//            dest.flush();
        }


        virtual void enterCompilationUnit(CajetaParser::CompilationUnitContext * /*ctx*/) override {
            cout << "enterCompilationUnit" << "\n";
        }
        virtual void exitCompilationUnit(CajetaParser::CompilationUnitContext * /*ctx*/) override {
            cout << "exitCompilationUnit" << "\n";
        }

        virtual void enterPackageDeclaration(CajetaParser::PackageDeclarationContext* ctx) override {
            std::vector<CajetaParser::IdentifierContext *> identifiers = ctx->qualifiedName()->identifier();
            auto itr = identifiers.begin();
            string packageName = (*itr)->getText();
            itr++;
            while (itr != identifiers.end()) {
                packageName += ".";
                packageName += (*itr)->getText();
                itr++;
            }

            // Ensure that the declared packagee canonical matches what we've got from the FS.
            if (compilationUnit->getQName() == nullptr || packageName != compilationUnit->getQName()->getPackageName()) {
                cerr << "Error: declared compilation unit package canonical does not match its location in source.";
            }
            cout << "enterPackageDeclaration" << "\n";
        }

        virtual void enterImportDeclaration(CajetaParser::ImportDeclarationContext* ctx) override {
            QualifiedName* qName = QualifiedName::fromContext(ctx->qualifiedName());
            compilationUnit->getImports()[qName->getTypeName()][qName->getPackageName()] = qName;
            cout << "enterImportDeclaration" << "\n";
        }

        virtual void enterTypeDeclaration(CajetaParser::TypeDeclarationContext * /*ctx*/) override {
            cout << "enterTypeDeclaration" << "\n";
        }
        virtual void exitTypeDeclaration(CajetaParser::TypeDeclarationContext *ctx) override {
            curStructure->getLlvmType();
            // TODO: Run a finish pass that will add fields to structures, blocks to methods / functions.
            cout << "exitTypeDeclaration" << ctx->getText() << "\n";
        }

        virtual void enterModifier(CajetaParser::ModifierContext * /*ctx*/) override {
            cout << "enterModifier" << "\n";
        }
        virtual void exitModifier(CajetaParser::ModifierContext * /*ctx*/) override {
            cout << "exitModifier" << "\n";
        }

        virtual void enterClassOrInterfaceModifier(CajetaParser::ClassOrInterfaceModifierContext* ctx) override {
            cout << "enterClassOrInterfaceModifier" << "\n";
            CajetaParser::AnnotationContext* ctxAnnotation = ctx->annotation();
            if (ctxAnnotation != nullptr) {
                QualifiedName* qName = QualifiedName::fromContext(ctxAnnotation->qualifiedName());
                curAnnotations.insert(qName);
            } else {
                modifiers.insert(Modifiable::toModifier(ctx->getText()));
            }
        }
        virtual void exitClassOrInterfaceModifier(CajetaParser::ClassOrInterfaceModifierContext * /*ctx*/) override {
            cout << "exitClassOrInterfaceModifier" << "\n";
        }

        virtual void enterVariableModifier(CajetaParser::VariableModifierContext * /*ctx*/) override {
            cout << "/*" << "\n";
        }
        virtual void exitVariableModifier(CajetaParser::VariableModifierContext * /*ctx*/) override {
            cout << "exitVariableModifier" << "\n";
        }

        // TODO: Make this a member variable (classDefinition), so that we can add fields to the type
        virtual void enterClassDeclaration(CajetaParser::ClassDeclarationContext* ctxClassDecl) override {
            cout << "enterClassDeclaration" << "\n";
            QualifiedName* qName = QualifiedName::create(ctxClassDecl->identifier()->getText(),
                                                         compilationUnit->getQName()->getPackageName());

            curStructure = new CajetaClass(llvmContext, qName, modifiers);
            types.push_back(curStructure);
            typeStack.push_front(curStructure);
            modifiers.clear();
        }

        virtual void exitClassDeclaration(CajetaParser::ClassDeclarationContext * /*ctx*/) override {
            cout << "exitClassDeclaration" << "\n";
            for (auto methodEntry : curStructure->getMethods()) {
                methodEntry.second->generate(llvmContext, *compilationUnit->getModule());
            }

        }

        virtual void enterTypeParameters(CajetaParser::TypeParametersContext * /*ctx*/) override {
            cout << "enterTypeParameters" << "\n";
        }
        virtual void exitTypeParameters(CajetaParser::TypeParametersContext * /*ctx*/) override {
            cout << "exitTypeParameters" << "\n";
        }

        virtual void enterTypeParameter(CajetaParser::TypeParameterContext * /*ctx*/) override {
            cout << "enterTypeParameter" << "\n";
        }
        virtual void exitTypeParameter(CajetaParser::TypeParameterContext * /*ctx*/) override {
            cout << "exitTypeParameter" << "\n";
        }

        virtual void enterTypeBound(CajetaParser::TypeBoundContext * /*ctx*/) override {
            cout << "enterTypeBound" << "\n";
        }
        virtual void exitTypeBound(CajetaParser::TypeBoundContext * /*ctx*/) override {
            cout << "exitTypeBound" << "\n";
        }

        virtual void enterEnumDeclaration(CajetaParser::EnumDeclarationContext * /*ctx*/) override {
            cout << "enterEnumDeclaration" << "\n";
        }
        virtual void exitEnumDeclaration(CajetaParser::EnumDeclarationContext * /*ctx*/) override {
            cout << "exitEnumDeclaration" << "\n";
        }

        virtual void enterEnumConstants(CajetaParser::EnumConstantsContext * /*ctx*/) override {
            cout << "enterEnumConstants" << "\n";
        }
        virtual void exitEnumConstants(CajetaParser::EnumConstantsContext * /*ctx*/) override {
            cout << "exitEnumConstants" << "\n";
        }

        virtual void enterEnumConstant(CajetaParser::EnumConstantContext * /*ctx*/) override {
            cout << "enterEnumConstant" << "\n";
        }
        virtual void exitEnumConstant(CajetaParser::EnumConstantContext * /*ctx*/) override {
            cout << "exitEnumConstant" << "\n";
        }

        virtual void enterEnumBodyDeclarations(CajetaParser::EnumBodyDeclarationsContext * /*ctx*/) override {
            cout << "enterEnumBodyDeclarations" << "\n";
        }
        virtual void exitEnumBodyDeclarations(CajetaParser::EnumBodyDeclarationsContext * /*ctx*/) override {
            cout << "exitEnumBodyDeclarations" << "\n";
        }

        virtual void enterInterfaceDeclaration(CajetaParser::InterfaceDeclarationContext * /*ctx*/) override {
            cout << "enterInterfaceDeclaration" << "\n";
        }
        virtual void exitInterfaceDeclaration(CajetaParser::InterfaceDeclarationContext * /*ctx*/) override {
            cout << "exitInterfaceDeclaration" << "\n";
        }

        virtual void enterClassBody(CajetaParser::ClassBodyContext * /*ctx*/) override {
            cout << "enterClassBody" << "\n";
        }
        virtual void exitClassBody(CajetaParser::ClassBodyContext * /*ctx*/) override {
            cout << "exitClassBody" << "\n";
        }

        virtual void enterInterfaceBody(CajetaParser::InterfaceBodyContext * /*ctx*/) override {
            cout << "enterInterfaceBody" << "\n";
        }
        virtual void exitInterfaceBody(CajetaParser::InterfaceBodyContext * /*ctx*/) override {
            cout << "exitInterfaceBody" << "\n";
        }

        virtual void enterClassBodyDeclaration(CajetaParser::ClassBodyDeclarationContext * /*ctx*/) override {
            cout << "enterClassBodyDeclaration" << "\n";
        }
        virtual void exitClassBodyDeclaration(CajetaParser::ClassBodyDeclarationContext * /*ctx*/) override {
            cout << "exitClassBodyDeclaration" << "\n";
        }

        virtual void enterMemberDeclaration(CajetaParser::MemberDeclarationContext* ctx) override {
            cout << "enterMemberDeclaration" << "\n";
        }
        virtual void exitMemberDeclaration(CajetaParser::MemberDeclarationContext * /*ctx*/) override {
            cout << "exitMemberDeclaration" << "\n";
        }

        /**
         *
         * @param ctx
         */
        virtual void enterMethodDeclaration(CajetaParser::MethodDeclarationContext *ctx) override {
            CajetaType* returnType = CajetaType::fromContext(ctx->typeTypeOrVoid());
            string name = ctx->identifier()->getText();
            bool constructor = name == curStructure->getQName()->getTypeName();
            list<FormalParameter*> parameters;

            CajetaParser::FormalParametersContext* ctxParameters = ctx->formalParameters();
            CajetaParser::FormalParameterListContext* ctxFormalParameterList = ctxParameters->formalParameterList();
            if (ctxFormalParameterList != nullptr) {
                std::vector<CajetaParser::FormalParameterContext*> formalParameters = ctxFormalParameterList->formalParameter();
                for (auto & ctxFormalParameter : formalParameters) {
                    FormalParameter* parameter = FormalParameter::fromContext(ctxFormalParameter);
                    if (parameter == nullptr) {
                        // TODO: Error reporting
                    } else {
                        parameters.push_back(parameter);
                    }
                }
            }

            curMethod = new Method(name, curStructure, returnType, parameters, modifiers, curAnnotations);
            curStructure->addMethod(curMethod);
            modifiers.clear();
            curAnnotations.clear();
        }

        /**
         *
         * @param ctxStatement
         */
        void processStatement(CajetaParser::StatementContext* ctxStatement) {
            std::vector<CajetaParser::StatementContext*> statements = ctxStatement->statement();
            for (auto itr = statements.begin(); itr != statements.end(); itr++) {
                CajetaParser::StatementContext* statementContext = *itr;
                if (statementContext->block()) {

                } else if (statementContext->IF()) {

                } else if (statementContext->FOR()) {

                } else if (statementContext->WHILE()) {

                } else if (statementContext->DO()) {

                } else if (statementContext->TRY()) {

                } else if (statementContext->SWITCH()) {

                } else if (statementContext->SYNCHRONIZED()) {

                } else if (statementContext->RETURN()) {

                } else if (statementContext->THROW()) {

                } else if (statementContext->BREAK()) {

                } else if (statementContext->CONTINUE()) {

                } else if (statementContext->YIELD()) {

                } else if (statementContext->SEMI()) {

                } else if (statementContext->statementExpression) {

                } else if (statementContext->switchExpression()) {

                } else if (statementContext->identifierLabel) {

                }
            }
        }

    /**
     * expression
    : primary
    | expression bop='.'
      (
         identifier
       | methodCall
       | THIS
       | NEW nonWildcardTypeArguments? innerCreator
       | SUPER superSuffix
       | explicitGenericInvocation
      )
    | expression '[' expression ']'
    | methodCall
    | NEW creator
    | '(' annotation* typeType ('&' typeType)* ')' expression
    | expression postfix=('++' | '--')
    | prefix=('+'|'-'|'++'|'--') expression
    | prefix=('~'|'!') expression
    | expression bop=('*'|'/'|'%') expression
    | expression bop=('+'|'-') expression
    | expression ('<' '<' | '>' '>' '>' | '>' '>') expression
    | expression bop=('<=' | '>=' | '>' | '<') expression
    | expression bop=INSTANCEOF (typeType | pattern)
    | expression bop=('==' | '!=') expression
    | expression bop='&' expression
    | expression bop='^' expression
    | expression bop='|' expression
    | expression bop='&&' expression
    | expression bop='||' expression
    | <assoc=right> expression bop='?' expression ':' expression
    | <assoc=right> expression
      bop=('=' | '+=' | '-=' | '*=' | '/=' | '&=' | '|=' | '^=' | '>>=' | '>>>=' | '<<=' | '%=')
      expression
    | lambdaExpression // Java8
    | switchExpression // Java17

    // Java 8 methodReference
    | expression '::' typeArguments? identifier
    | typeType '::' (typeArguments? identifier | NEW)
    | classType '::' typeArguments? NEW
    ;

         * @param ctxExpression
         */
        void processExpression(CajetaParser::ExpressionContext* ctxExpression) {
            cout << "Entered processExpression" << "\n";
        }
        virtual void exitMethodDeclaration(CajetaParser::MethodDeclarationContext * /*ctx*/) override {
            cout << "exitMethodDeclaration" << "\n";
        }

        virtual void enterMethodBody(CajetaParser::MethodBodyContext * /*ctx*/) override {
            cout << "enterMethodBody" << "\n";
        }
        virtual void exitMethodBody(CajetaParser::MethodBodyContext * /*ctx*/) override {
            cout << "exitMethodBody" << "\n";
        }

        virtual void enterTypeTypeOrVoid(CajetaParser::TypeTypeOrVoidContext * /*ctx*/) override {
            cout << "enterTypeTypeOrVoid" << "\n";
        }
        virtual void exitTypeTypeOrVoid(CajetaParser::TypeTypeOrVoidContext * /*ctx*/) override {
            cout << "exitTypeTypeOrVoid" << "\n";
        }

        virtual void enterGenericMethodDeclaration(CajetaParser::GenericMethodDeclarationContext * /*ctx*/) override {
            cout << "enterGenericMethodDeclaration" << "\n";
        }
        virtual void exitGenericMethodDeclaration(CajetaParser::GenericMethodDeclarationContext * /*ctx*/) override {
            cout << "exitGenericMethodDeclaration" << "\n";
        }

        virtual void enterGenericConstructorDeclaration(CajetaParser::GenericConstructorDeclarationContext * /*ctx*/) override {
            cout << "enterGenericConstructorDeclaration" << "\n";
        }
        virtual void exitGenericConstructorDeclaration(CajetaParser::GenericConstructorDeclarationContext * /*ctx*/) override {
            cout << "exitGenericConstructorDeclaration" << "\n";
        }

        virtual void enterConstructorDeclaration(CajetaParser::ConstructorDeclarationContext * /*ctx*/) override {
            cout << "enterConstructorDeclaration" << "\n";
        }
        virtual void exitConstructorDeclaration(CajetaParser::ConstructorDeclarationContext * /*ctx*/) override {
            cout << "exitConstructorDeclaration" << "\n";
        }

        virtual void enterFieldDeclaration(CajetaParser::FieldDeclarationContext* ctx) override {
            list<Field*> fields = Field::fromContext(ctx);
            for (Field* field : fields) {
                field->addModifiers(modifiers);
            }
            modifiers.clear();
            curStructure->addFields(fields);
            cout << "enterFieldDeclaration" << "\n";
        }
        virtual void exitFieldDeclaration(CajetaParser::FieldDeclarationContext * /*ctx*/) override {
            cout << "exitFieldDeclaration" << "\n";
        }

        virtual void enterInterfaceBodyDeclaration(CajetaParser::InterfaceBodyDeclarationContext * /*ctx*/) override {
            cout << "enterInterfaceBodyDeclaration" << "\n";
        }
        virtual void exitInterfaceBodyDeclaration(CajetaParser::InterfaceBodyDeclarationContext * /*ctx*/) override {
            cout << "exitInterfaceBodyDeclaration" << "\n";
        }

        virtual void enterInterfaceMemberDeclaration(CajetaParser::InterfaceMemberDeclarationContext * /*ctx*/) override {
            cout << "enterInterfaceMemberDeclaration" << "\n";
        }
        virtual void exitInterfaceMemberDeclaration(CajetaParser::InterfaceMemberDeclarationContext * /*ctx*/) override {
            cout << "exitInterfaceMemberDeclaration" << "\n";
        }

        virtual void enterConstDeclaration(CajetaParser::ConstDeclarationContext * /*ctx*/) override {
            cout << "enterConstDeclaration" << "\n";
        }
        virtual void exitConstDeclaration(CajetaParser::ConstDeclarationContext * /*ctx*/) override {
            cout << "exitConstDeclaration" << "\n";
        }

        virtual void enterConstantDeclarator(CajetaParser::ConstantDeclaratorContext * /*ctx*/) override {
            cout << "enterConstantDeclarator" << "\n";
        }
        virtual void exitConstantDeclarator(CajetaParser::ConstantDeclaratorContext * /*ctx*/) override {
            cout << "exitConstantDeclarator" << "\n";
        }

        virtual void enterInterfaceMethodDeclaration(CajetaParser::InterfaceMethodDeclarationContext * /*ctx*/) override {
            cout << "enterInterfaceMethodDeclaration" << "\n";
        }
        virtual void exitInterfaceMethodDeclaration(CajetaParser::InterfaceMethodDeclarationContext * /*ctx*/) override {
            cout << "exitInterfaceMethodDeclaration" << "\n";
        }

        virtual void enterInterfaceMethodModifier(CajetaParser::InterfaceMethodModifierContext * /*ctx*/) override {
            cout << "enterInterfaceMethodModifier" << "\n";
        }
        virtual void exitInterfaceMethodModifier(CajetaParser::InterfaceMethodModifierContext * /*ctx*/) override {
            cout << "exitInterfaceMethodModifier" << "\n";
        }

        virtual void enterGenericInterfaceMethodDeclaration(CajetaParser::GenericInterfaceMethodDeclarationContext * /*ctx*/) override {
            cout << "enterGenericInterfaceMethodDeclaration" << "\n";
        }
        virtual void exitGenericInterfaceMethodDeclaration(CajetaParser::GenericInterfaceMethodDeclarationContext * /*ctx*/) override {
            cout << "exitGenericInterfaceMethodDeclaration" << "\n";
        }

        virtual void enterInterfaceCommonBodyDeclaration(CajetaParser::InterfaceCommonBodyDeclarationContext * /*ctx*/) override {
            cout << "enterInterfaceCommonBodyDeclaration" << "\n";
        }
        virtual void exitInterfaceCommonBodyDeclaration(CajetaParser::InterfaceCommonBodyDeclarationContext * /*ctx*/) override {
            cout << "exitInterfaceCommonBodyDeclaration" << "\n";
        }

        virtual void enterVariableDeclarators(CajetaParser::VariableDeclaratorsContext * /*ctx*/) override {
            cout << "enterVariableDeclarators" << "\n";
        }
        virtual void exitVariableDeclarators(CajetaParser::VariableDeclaratorsContext * /*ctx*/) override {
            cout << "exitVariableDeclarators" << "\n";
        }

        virtual void enterVariableDeclarator(CajetaParser::VariableDeclaratorContext * /*ctx*/) override {
            cout << "enterVariableDeclarator" << "\n";
        }
        virtual void exitVariableDeclarator(CajetaParser::VariableDeclaratorContext * /*ctx*/) override {
            cout << "exitVariableDeclarator" << "\n";
        }

        virtual void enterVariableDeclaratorId(CajetaParser::VariableDeclaratorIdContext * /*ctx*/) override {
            cout << "enterVariableDeclaratorId" << "\n";
        }
        virtual void exitVariableDeclaratorId(CajetaParser::VariableDeclaratorIdContext * /*ctx*/) override {
            cout << "exitVariableDeclaratorId" << "\n";
        }

        virtual void enterVariableInitializer(CajetaParser::VariableInitializerContext * /*ctx*/) override {
            cout << "enterVariableInitializer" << "\n";
        }
        virtual void exitVariableInitializer(CajetaParser::VariableInitializerContext * /*ctx*/) override {
            cout << "exitVariableInitializer" << "\n";
        }

        virtual void enterArrayInitializer(CajetaParser::ArrayInitializerContext * /*ctx*/) override {
            cout << "enterArrayInitializer" << "\n";
        }
        virtual void exitArrayInitializer(CajetaParser::ArrayInitializerContext * /*ctx*/) override {
            cout << "exitArrayInitializer" << "\n";
        }

        virtual void enterClassOrInterfaceType(CajetaParser::ClassOrInterfaceTypeContext * /*ctx*/) override {
            cout << "enterClassOrInterfaceType" << "\n";
        }
        virtual void exitClassOrInterfaceType(CajetaParser::ClassOrInterfaceTypeContext * /*ctx*/) override {
            cout << "exitClassOrInterfaceType" << "\n";
        }

        virtual void enterTypeArgument(CajetaParser::TypeArgumentContext * /*ctx*/) override {
            cout << "enterTypeArgument" << "\n";
        }
        virtual void exitTypeArgument(CajetaParser::TypeArgumentContext * /*ctx*/) override {
            cout << "exitTypeArgument" << "\n";
        }

        virtual void enterQualifiedNameList(CajetaParser::QualifiedNameListContext * /*ctx*/) override {
            cout << "enterQualifiedNameList" << "\n";
        }
        virtual void exitQualifiedNameList(CajetaParser::QualifiedNameListContext * /*ctx*/) override {
            cout << "exitQualifiedNameList" << "\n";
        }

        virtual void enterFormalParameters(CajetaParser::FormalParametersContext * /*ctx*/) override {
            cout << "enterFormalParameters" << "\n";
        }
        virtual void exitFormalParameters(CajetaParser::FormalParametersContext * /*ctx*/) override {
            cout << "exitFormalParameters" << "\n";
        }

        virtual void enterReceiverParameter(CajetaParser::ReceiverParameterContext * /*ctx*/) override {
            cout << "enterReceiverParameter" << "\n";
        }
        virtual void exitReceiverParameter(CajetaParser::ReceiverParameterContext * /*ctx*/) override {
            cout << "exitReceiverParameter" << "\n";
        }

        virtual void enterFormalParameterList(CajetaParser::FormalParameterListContext* ctx) override {
            cout << "enterFormalParameterList" << "\n";

        }
        virtual void exitFormalParameterList(CajetaParser::FormalParameterListContext * /*ctx*/) override {
            cout << "exitFormalParameterList" << "\n";
        }

        virtual void enterFormalParameter(CajetaParser::FormalParameterContext * /*ctx*/) override {
            cout << "enterFormalParameter" << "\n";
        }
        virtual void exitFormalParameter(CajetaParser::FormalParameterContext * /*ctx*/) override {
            cout << "exitFormalParameter" << "\n";
        }

        virtual void enterLastFormalParameter(CajetaParser::LastFormalParameterContext * /*ctx*/) override {
            cout << "enterLastFormalParameter" << "\n";
        }
        virtual void exitLastFormalParameter(CajetaParser::LastFormalParameterContext * /*ctx*/) override {
            cout << "exitLastFormalParameter" << "\n";
        }

        virtual void enterLambdaLVTIList(CajetaParser::LambdaLVTIListContext * /*ctx*/) override {
            cout << "enterLambdaLVTIList" << "\n";
        }
        virtual void exitLambdaLVTIList(CajetaParser::LambdaLVTIListContext * /*ctx*/) override {
            cout << "exitLambdaLVTIList" << "\n";
        }

        virtual void enterLambdaLVTIParameter(CajetaParser::LambdaLVTIParameterContext * /*ctx*/) override {
            cout << "enterLambdaLVTIParameter" << "\n";
        }
        virtual void exitLambdaLVTIParameter(CajetaParser::LambdaLVTIParameterContext * /*ctx*/) override {
            cout << "exitLambdaLVTIParameter" << "\n";
        }

        virtual void enterQualifiedName(CajetaParser::QualifiedNameContext * /*ctx*/) override {
            cout << "enterQualifiedName" << "\n";
        }
        virtual void exitQualifiedName(CajetaParser::QualifiedNameContext * /*ctx*/) override {
            cout << "exitQualifiedName" << "\n";
        }

        virtual void enterLiteral(CajetaParser::LiteralContext * /*ctx*/) override {
            cout << "enterLiteral" << "\n";
        }
        virtual void exitLiteral(CajetaParser::LiteralContext * /*ctx*/) override {
            cout << "exitLiteral" << "\n";
        }

        virtual void enterIntegerLiteral(CajetaParser::IntegerLiteralContext * /*ctx*/) override {
            cout << "enterIntegerLiteral" << "\n";
        }
        virtual void exitIntegerLiteral(CajetaParser::IntegerLiteralContext * /*ctx*/) override {
            cout << "exitIntegerLiteral" << "\n";
        }

        virtual void enterFloatLiteral(CajetaParser::FloatLiteralContext * /*ctx*/) override {
            cout << "enterFloatLiteral" << "\n";
        }
        virtual void exitFloatLiteral(CajetaParser::FloatLiteralContext * /*ctx*/) override {
            cout << "exitFloatLiteral" << "\n";
        }

        virtual void enterAltAnnotationQualifiedName(CajetaParser::AltAnnotationQualifiedNameContext * /*ctx*/) override {
            cout << "enterAltAnnotationQualifiedName" << "\n";
        }
        virtual void exitAltAnnotationQualifiedName(CajetaParser::AltAnnotationQualifiedNameContext * /*ctx*/) override {
            cout << "exitAltAnnotationQualifiedName" << "\n";
        }

        virtual void enterAnnotation(CajetaParser::AnnotationContext * /*ctx*/) override {
            cout << "enterAnnotation" << "\n";
        }
        virtual void exitAnnotation(CajetaParser::AnnotationContext * /*ctx*/) override {
            cout << "exitAnnotation" << "\n";
        }

        virtual void enterElementValuePairs(CajetaParser::ElementValuePairsContext * /*ctx*/) override {
            cout << "enterElementValuePairs" << "\n";
        }
        virtual void exitElementValuePairs(CajetaParser::ElementValuePairsContext * /*ctx*/) override {
            cout << "exitElementValuePairs" << "\n";
        }

        virtual void enterElementValuePair(CajetaParser::ElementValuePairContext * /*ctx*/) override {
            cout << "enterElementValuePair" << "\n";
        }
        virtual void exitElementValuePair(CajetaParser::ElementValuePairContext * /*ctx*/) override {
            cout << "exitElementValuePair" << "\n";
        }

        virtual void enterElementValue(CajetaParser::ElementValueContext * /*ctx*/) override {
            cout << "enterElementValue" << "\n";
        }
        virtual void exitElementValue(CajetaParser::ElementValueContext * /*ctx*/) override {
            cout << "exitElementValue" << "\n";
        }

        virtual void enterElementValueArrayInitializer(CajetaParser::ElementValueArrayInitializerContext * /*ctx*/) override {
            cout << "enterElementValueArrayInitializer" << "\n";
        }
        virtual void exitElementValueArrayInitializer(CajetaParser::ElementValueArrayInitializerContext * /*ctx*/) override {
            cout << "exitElementValueArrayInitializer" << "\n";
        }

        virtual void enterAnnotationTypeDeclaration(CajetaParser::AnnotationTypeDeclarationContext * /*ctx*/) override {
            cout << "enterAnnotationTypeDeclaration" << "\n";
        }
        virtual void exitAnnotationTypeDeclaration(CajetaParser::AnnotationTypeDeclarationContext * /*ctx*/) override {
            cout << "exitAnnotationTypeDeclaration" << "\n";
        }

        virtual void enterAnnotationTypeBody(CajetaParser::AnnotationTypeBodyContext * /*ctx*/) override {
            cout << "enterAnnotationTypeBody" << "\n";
        }
        virtual void exitAnnotationTypeBody(CajetaParser::AnnotationTypeBodyContext * /*ctx*/) override {
            cout << "exitAnnotationTypeBody" << "\n";
        }

        virtual void enterAnnotationTypeElementDeclaration(CajetaParser::AnnotationTypeElementDeclarationContext * /*ctx*/) override {
            cout << "enterAnnotationTypeElementDeclaration" << "\n";
        }
        virtual void exitAnnotationTypeElementDeclaration(CajetaParser::AnnotationTypeElementDeclarationContext * /*ctx*/) override {
            cout << "exitAnnotationTypeElementDeclaration" << "\n";
        }

        virtual void enterAnnotationTypeElementRest(CajetaParser::AnnotationTypeElementRestContext * /*ctx*/) override {
            cout << "enterAnnotationTypeElementRest" << "\n";
        }
        virtual void exitAnnotationTypeElementRest(CajetaParser::AnnotationTypeElementRestContext * /*ctx*/) override {
            cout << "exitAnnotationTypeElementRest" << "\n";
        }

        virtual void enterAnnotationMethodOrConstantRest(CajetaParser::AnnotationMethodOrConstantRestContext * /*ctx*/) override {
            cout << "enterAnnotationMethodOrConstantRest" << "\n";
        }
        virtual void exitAnnotationMethodOrConstantRest(CajetaParser::AnnotationMethodOrConstantRestContext * /*ctx*/) override {
            cout << "exitAnnotationMethodOrConstantRest" << "\n";
        }

        virtual void enterAnnotationMethodRest(CajetaParser::AnnotationMethodRestContext * /*ctx*/) override {
            cout << "enterAnnotationMethodRest" << "\n";
        }
        virtual void exitAnnotationMethodRest(CajetaParser::AnnotationMethodRestContext * /*ctx*/) override {
            cout << "exitAnnotationMethodRest" << "\n";
        }

        virtual void enterAnnotationConstantRest(CajetaParser::AnnotationConstantRestContext * /*ctx*/) override {
            cout << "enterAnnotationConstantRest" << "\n";
        }
        virtual void exitAnnotationConstantRest(CajetaParser::AnnotationConstantRestContext * /*ctx*/) override {
            cout << "exitAnnotationConstantRest" << "\n";
        }

        virtual void enterDefaultValue(CajetaParser::DefaultValueContext * /*ctx*/) override {
            cout << "enterDefaultValue" << "\n";
        }
        virtual void exitDefaultValue(CajetaParser::DefaultValueContext * /*ctx*/) override {
            cout << "exitDefaultValue" << "\n";
        }

        virtual void enterModuleDeclaration(CajetaParser::ModuleDeclarationContext * /*ctx*/) override {
            cout << "enterModuleDeclaration" << "\n";
        }
        virtual void exitModuleDeclaration(CajetaParser::ModuleDeclarationContext * /*ctx*/) override {
            cout << "exitModuleDeclaration" << "\n";
        }

        virtual void enterModuleBody(CajetaParser::ModuleBodyContext * /*ctx*/) override {
            cout << "enterModuleBody" << "\n";
        }
        virtual void exitModuleBody(CajetaParser::ModuleBodyContext * /*ctx*/) override {
            cout << "exitModuleBody" << "\n";
        }

        virtual void enterModuleDirective(CajetaParser::ModuleDirectiveContext * /*ctx*/) override {
            cout << "enterModuleDirective" << "\n";
        }
        virtual void exitModuleDirective(CajetaParser::ModuleDirectiveContext * /*ctx*/) override {
            cout << "exitModuleDirective" << "\n";
        }

        virtual void enterRequiresModifier(CajetaParser::RequiresModifierContext * /*ctx*/) override {
            cout << "enterRequiresModifier" << "\n";
        }
        virtual void exitRequiresModifier(CajetaParser::RequiresModifierContext * /*ctx*/) override {
            cout << "exitRequiresModifier" << "\n";
        }

        virtual void enterRecordDeclaration(CajetaParser::RecordDeclarationContext * /*ctx*/) override {
            cout << "enterRecordDeclaration" << "\n";
        }
        virtual void exitRecordDeclaration(CajetaParser::RecordDeclarationContext * /*ctx*/) override {
            cout << "exitRecordDeclaration" << "\n";
        }

        virtual void enterRecordHeader(CajetaParser::RecordHeaderContext * /*ctx*/) override {
            cout << "enterRecordHeader" << "\n";
        }
        virtual void exitRecordHeader(CajetaParser::RecordHeaderContext * /*ctx*/) override {
            cout << "exitRecordHeader" << "\n";
        }

        virtual void enterRecordComponentList(CajetaParser::RecordComponentListContext * /*ctx*/) override {
            cout << "enterRecordComponentList" << "\n";
        }
        virtual void exitRecordComponentList(CajetaParser::RecordComponentListContext * /*ctx*/) override {
            cout << "exitRecordComponentList" << "\n";
        }

        virtual void enterRecordComponent(CajetaParser::RecordComponentContext * /*ctx*/) override {
            cout << "enterRecordComponent" << "\n";
        }
        virtual void exitRecordComponent(CajetaParser::RecordComponentContext * /*ctx*/) override {
            cout << "exitRecordComponent" << "\n";
        }

        virtual void enterRecordBody(CajetaParser::RecordBodyContext * /*ctx*/) override {
            cout << "enterRecordBody" << "\n";
        }
        virtual void exitRecordBody(CajetaParser::RecordBodyContext * /*ctx*/) override {
            cout << "exitRecordBody" << "\n";
        }

        virtual void enterBlock(CajetaParser::BlockContext * /*ctx*/) override {
            curMethod->addBlock(new Block(llvmContext, ));
            cout << "enterBlock" << "\n";
        }
        virtual void exitBlock(CajetaParser::BlockContext * /*ctx*/) override {
            cout << "exitBlock" << "\n";
        }

        virtual void enterBlockStatement(CajetaParser::BlockStatementContext * /*ctx*/) override {
            cout << "enterBlockStatement" << "\n";
        }
        virtual void exitBlockStatement(CajetaParser::BlockStatementContext * /*ctx*/) override {
            cout << "exitBlockStatement" << "\n";
        }

        virtual void enterLocalVariableDeclaration(CajetaParser::LocalVariableDeclarationContext * /*ctx*/) override {
            cout << "enterLocalVariableDeclaration" << "\n";
        }
        virtual void exitLocalVariableDeclaration(CajetaParser::LocalVariableDeclarationContext * /*ctx*/) override {
            cout << "exitLocalVariableDeclaration" << "\n";
        }

        virtual void enterIdentifier(CajetaParser::IdentifierContext * /*ctx*/) override {
            cout << "enterIdentifier" << "\n";
        }
        virtual void exitIdentifier(CajetaParser::IdentifierContext * /*ctx*/) override {
            cout << "exitIdentifier" << "\n";
        }

        virtual void enterLocalTypeDeclaration(CajetaParser::LocalTypeDeclarationContext * /*ctx*/) override {
            cout << "enterLocalTypeDeclaration" << "\n";
        }
        virtual void exitLocalTypeDeclaration(CajetaParser::LocalTypeDeclarationContext * /*ctx*/) override {
            cout << "exitLocalTypeDeclaration" << "\n";
        }

        virtual void enterStatement(CajetaParser::StatementContext * /*ctx*/) override {

            cout << "enterStatement" << "\n";
        }
        virtual void exitStatement(CajetaParser::StatementContext * /*ctx*/) override {
            cout << "exitStatement" << "\n";
        }

        virtual void enterCatchClause(CajetaParser::CatchClauseContext * /*ctx*/) override {
            cout << "enterCatchClause" << "\n";
        }
        virtual void exitCatchClause(CajetaParser::CatchClauseContext * /*ctx*/) override {
            cout << "exitCatchClause" << "\n";
        }

        virtual void enterCatchType(CajetaParser::CatchTypeContext * /*ctx*/) override {
            cout << "enterCatchType" << "\n";
        }
        virtual void exitCatchType(CajetaParser::CatchTypeContext * /*ctx*/) override {
            cout << "exitCatchType" << "\n";
        }

        virtual void enterFinallyBlock(CajetaParser::FinallyBlockContext * /*ctx*/) override {
            cout << "enterFinallyBlock" << "\n";
        }
        virtual void exitFinallyBlock(CajetaParser::FinallyBlockContext * /*ctx*/) override {
            cout << "exitFinallyBlock" << "\n";
        }

        virtual void enterResourceSpecification(CajetaParser::ResourceSpecificationContext * /*ctx*/) override {
            cout << "enterResourceSpecification" << "\n";
        }
        virtual void exitResourceSpecification(CajetaParser::ResourceSpecificationContext * /*ctx*/) override {
            cout << "exitResourceSpecification" << "\n";
        }

        virtual void enterResources(CajetaParser::ResourcesContext * /*ctx*/) override {
            cout << "enterResources" << "\n";
        }
        virtual void exitResources(CajetaParser::ResourcesContext * /*ctx*/) override {
            cout << "exitResources" << "\n";
        }

        virtual void enterResource(CajetaParser::ResourceContext * /*ctx*/) override {
            cout << "enterResource" << "\n";
        }
        virtual void exitResource(CajetaParser::ResourceContext * /*ctx*/) override {
            cout << "exitResource" << "\n";
        }

        virtual void enterSwitchBlockStatementGroup(CajetaParser::SwitchBlockStatementGroupContext * /*ctx*/) override {
            cout << "enterSwitchBlockStatementGroup" << "\n";
        }
        virtual void exitSwitchBlockStatementGroup(CajetaParser::SwitchBlockStatementGroupContext * /*ctx*/) override {
            cout << "exitSwitchBlockStatementGroup" << "\n";
        }

        virtual void enterSwitchLabel(CajetaParser::SwitchLabelContext * /*ctx*/) override {
            cout << "enterSwitchLabel" << "\n";
        }
        virtual void exitSwitchLabel(CajetaParser::SwitchLabelContext * /*ctx*/) override {
            cout << "exitSwitchLabel" << "\n";
        }

        virtual void enterForControl(CajetaParser::ForControlContext * /*ctx*/) override {
            cout << "enterForControl" << "\n";
        }
        virtual void exitForControl(CajetaParser::ForControlContext * /*ctx*/) override {
            cout << "exitForControl" << "\n";
        }

        virtual void enterForInit(CajetaParser::ForInitContext * /*ctx*/) override {
            cout << "enterForInit" << "\n";
        }
        virtual void exitForInit(CajetaParser::ForInitContext * /*ctx*/) override {
            cout << "exitForInit" << "\n";
        }

        virtual void enterEnhancedForControl(CajetaParser::EnhancedForControlContext * /*ctx*/) override {
            cout << "enterEnhancedForControl" << "\n";
        }
        virtual void exitEnhancedForControl(CajetaParser::EnhancedForControlContext * /*ctx*/) override {
            cout << "exitEnhancedForControl" << "\n";
        }

        virtual void enterParExpression(CajetaParser::ParExpressionContext * /*ctx*/) override {
            cout << "enterParExpression" << "\n";
        }
        virtual void exitParExpression(CajetaParser::ParExpressionContext * /*ctx*/) override {
            cout << "exitParExpression" << "\n";
        }

        virtual void enterExpressionList(CajetaParser::ExpressionListContext* ctx) override {
            cout << "enterExpressionList" << "\n";
            cout << "enterMethodCall" << ctx->getText() << "\n";
        }
        virtual void exitExpressionList(CajetaParser::ExpressionListContext * /*ctx*/) override {
            cout << "exitExpressionList" << "\n";
        }

        virtual void enterMethodCall(CajetaParser::MethodCallContext* ctx) override {
            string str = ctx->getText();
            cout << "enterMethodCall" << ctx->getText() << "\n";
        }
        virtual void exitMethodCall(CajetaParser::MethodCallContext * /*ctx*/) override {
            cout << "exitMethodCall" << "\n";
        }

        virtual void enterExpression(CajetaParser::ExpressionContext* ctx) override {
            cout << "enterExpression" << ctx->getText() << "\n";
        }
        virtual void exitExpression(CajetaParser::ExpressionContext * /*ctx*/) override {
            cout << "exitExpression" << "\n";
        }

        virtual void enterPattern(CajetaParser::PatternContext * /*ctx*/) override {
            cout << "enterPattern" << "\n";
        }
        virtual void exitPattern(CajetaParser::PatternContext * /*ctx*/) override {
            cout << "exitPattern" << "\n";
        }

        virtual void enterLambdaExpression(CajetaParser::LambdaExpressionContext * /*ctx*/) override {
            cout << "enterLambdaExpression" << "\n";
        }
        virtual void exitLambdaExpression(CajetaParser::LambdaExpressionContext * /*ctx*/) override {
            cout << "exitLambdaExpression" << "\n";
        }

        virtual void enterLambdaParameters(CajetaParser::LambdaParametersContext * /*ctx*/) override {
            cout << "enterLambdaParameters" << "\n";
        }
        virtual void exitLambdaParameters(CajetaParser::LambdaParametersContext * /*ctx*/) override {
            cout << "exitLambdaParameters" << "\n";
        }

        virtual void enterLambdaBody(CajetaParser::LambdaBodyContext * /*ctx*/) override {
            cout << "enterLambdaBody" << "\n";
        }
        virtual void exitLambdaBody(CajetaParser::LambdaBodyContext * /*ctx*/) override {
            cout << "exitLambdaBody" << "\n";
        }

        virtual void enterPrimary(CajetaParser::PrimaryContext * /*ctx*/) override {
            cout << "enterPrimary" << "\n";
        }
        virtual void exitPrimary(CajetaParser::PrimaryContext * /*ctx*/) override {
            cout << "exitPrimary" << "\n";
        }

        virtual void enterSwitchExpression(CajetaParser::SwitchExpressionContext * /*ctx*/) override {
            cout << "enterSwitchExpression" << "\n";
        }
        virtual void exitSwitchExpression(CajetaParser::SwitchExpressionContext * /*ctx*/) override {
            cout << "exitSwitchExpression" << "\n";
        }

        virtual void enterSwitchLabeledRule(CajetaParser::SwitchLabeledRuleContext * /*ctx*/) override {
            cout << "enterSwitchLabeledRule" << "\n";
        }
        virtual void exitSwitchLabeledRule(CajetaParser::SwitchLabeledRuleContext * /*ctx*/) override {
            cout << "exitSwitchLabeledRule" << "\n";
        }

        virtual void enterGuardedPattern(CajetaParser::GuardedPatternContext * /*ctx*/) override {
            cout << "enterGuardedPattern" << "\n";
        }
        virtual void exitGuardedPattern(CajetaParser::GuardedPatternContext * /*ctx*/) override {
            cout << "exitGuardedPattern" << "\n";
        }

        virtual void enterSwitchRuleOutcome(CajetaParser::SwitchRuleOutcomeContext * /*ctx*/) override {
            cout << "enterSwitchRuleOutcome" << "\n";
        }
        virtual void exitSwitchRuleOutcome(CajetaParser::SwitchRuleOutcomeContext * /*ctx*/) override {
            cout << "exitSwitchRuleOutcome" << "\n";
        }

        virtual void enterClassType(CajetaParser::ClassTypeContext * /*ctx*/) override {
            cout << "enterClassType" << "\n";
        }
        virtual void exitClassType(CajetaParser::ClassTypeContext * /*ctx*/) override {
            cout << "exitClassType" << "\n";
        }

        virtual void enterCreator(CajetaParser::CreatorContext * /*ctx*/) override {
            cout << "enterCreator" << "\n";
        }
        virtual void exitCreator(CajetaParser::CreatorContext * /*ctx*/) override {
            cout << "exitCreator" << "\n";
        }

        virtual void enterCreatedName(CajetaParser::CreatedNameContext * /*ctx*/) override {
            cout << "enterCreatedName" << "\n";
        }
        virtual void exitCreatedName(CajetaParser::CreatedNameContext * /*ctx*/) override {
            cout << "exitCreatedName" << "\n";
        }

        virtual void enterInnerCreator(CajetaParser::InnerCreatorContext * /*ctx*/) override {
            cout << "enterInnerCreator" << "\n";
        }
        virtual void exitInnerCreator(CajetaParser::InnerCreatorContext * /*ctx*/) override {
            cout << "exitInnerCreator" << "\n";
        }

        virtual void enterArrayCreatorRest(CajetaParser::ArrayCreatorRestContext * /*ctx*/) override {
            cout << "enterArrayCreatorRest" << "\n";
        }
        virtual void exitArrayCreatorRest(CajetaParser::ArrayCreatorRestContext * /*ctx*/) override {
            cout << "exitArrayCreatorRest" << "\n";
        }

        virtual void enterClassCreatorRest(CajetaParser::ClassCreatorRestContext * /*ctx*/) override {
            cout << "enterClassCreatorRest" << "\n";
        }
        virtual void exitClassCreatorRest(CajetaParser::ClassCreatorRestContext * /*ctx*/) override {
            cout << "exitClassCreatorRest" << "\n";
        }

        virtual void enterExplicitGenericInvocation(CajetaParser::ExplicitGenericInvocationContext * /*ctx*/) override {
            cout << "enterExplicitGenericInvocation" << "\n";
        }
        virtual void exitExplicitGenericInvocation(CajetaParser::ExplicitGenericInvocationContext * /*ctx*/) override {
            cout << "exitExplicitGenericInvocation" << "\n";
        }

        virtual void enterTypeArgumentsOrDiamond(CajetaParser::TypeArgumentsOrDiamondContext * /*ctx*/) override {
            cout << "enterTypeArgumentsOrDiamond" << "\n";
        }
        virtual void exitTypeArgumentsOrDiamond(CajetaParser::TypeArgumentsOrDiamondContext * /*ctx*/) override {
            cout << "exitTypeArgumentsOrDiamond" << "\n";
        }

        virtual void enterNonWildcardTypeArgumentsOrDiamond(CajetaParser::NonWildcardTypeArgumentsOrDiamondContext * /*ctx*/) override {
            cout << "enterNonWildcardTypeArgumentsOrDiamond" << "\n";
        }
        virtual void exitNonWildcardTypeArgumentsOrDiamond(CajetaParser::NonWildcardTypeArgumentsOrDiamondContext * /*ctx*/) override {
            cout << "exitNonWildcardTypeArgumentsOrDiamond" << "\n";
        }

        virtual void enterNonWildcardTypeArguments(CajetaParser::NonWildcardTypeArgumentsContext * /*ctx*/) override {
            cout << "enterNonWildcardTypeArguments" << "\n";
        }
        virtual void exitNonWildcardTypeArguments(CajetaParser::NonWildcardTypeArgumentsContext * /*ctx*/) override {
            cout << "exitNonWildcardTypeArguments" << "\n";
        }

        virtual void enterTypeList(CajetaParser::TypeListContext * /*ctx*/) override {
            cout << "enterTypeList" << "\n";
        }
        virtual void exitTypeList(CajetaParser::TypeListContext * /*ctx*/) override {
            cout << "exitTypeList" << "\n";
        }

        virtual void enterTypeType(CajetaParser::TypeTypeContext * /*ctx*/) override {
            cout << "enterTypeType" << "\n";
        }
        virtual void exitTypeType(CajetaParser::TypeTypeContext * /*ctx*/) override {
            cout << "exitTypeType" << "\n";
        }

        virtual void enterPrimitiveType(CajetaParser::PrimitiveTypeContext * /*ctx*/) override {

            cout << "enterPrimitiveType" << "\n";
        }
        virtual void exitPrimitiveType(CajetaParser::PrimitiveTypeContext * /*ctx*/) override {
            cout << "exitPrimitiveType" << "\n";
        }

        virtual void enterTypeArguments(CajetaParser::TypeArgumentsContext * /*ctx*/) override {
            cout << "enterTypeArguments" << "\n";
        }
        virtual void exitTypeArguments(CajetaParser::TypeArgumentsContext * /*ctx*/) override {
            cout << "exitTypeArguments" << "\n";
        }

        virtual void enterSuperSuffix(CajetaParser::SuperSuffixContext * /*ctx*/) override {
            cout << "enterSuperSuffix" << "\n";
        }
        virtual void exitSuperSuffix(CajetaParser::SuperSuffixContext * /*ctx*/) override {
            cout << "exitSuperSuffix" << "\n";
        }

        virtual void enterExplicitGenericInvocationSuffix(CajetaParser::ExplicitGenericInvocationSuffixContext * /*ctx*/) override {
            cout << "enterExplicitGenericInvocationSuffix" << "\n";
        }
        virtual void exitExplicitGenericInvocationSuffix(CajetaParser::ExplicitGenericInvocationSuffixContext * /*ctx*/) override {
            cout << "exitExplicitGenericInvocationSuffix" << "\n";
        }

        virtual void enterArguments(CajetaParser::ArgumentsContext * /*ctx*/) override {
            cout << "enterArguments" << "\n";
        }
        virtual void exitArguments(CajetaParser::ArgumentsContext * /*ctx*/) override {
            cout << "exitArguments" << "\n";
        }

        virtual void enterEveryRule(antlr4::ParserRuleContext * /*ctx*/) override {
            cout << "void" << "\n";
        }
        virtual void exitEveryRule(antlr4::ParserRuleContext * /*ctx*/) override {
            cout << "exitEveryRule" << "\n";
        }

        virtual void visitTerminal(antlr4::tree::TerminalNode * /*node*/) override {
            cout << "visitTerminal" << "\n";
        }
        virtual void visitErrorNode(antlr4::tree::ErrorNode* node) override {
            string str = node->getText();
            cout << "visitErrorNode: " << str << "\n";
        }
    };
}