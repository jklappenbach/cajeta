#pragma once

#include <llvm/Target/TargetOptions.h>
#include "CajetaParser.h"
#include <llvm/Bitcode/BitcodeWriter.h>
#include "CajetaParserBaseListener.h"
#include <llvm/ADT/StringRef.h>
#include "cajeta/asn/Statement.h"
#include "cajeta/type/Generics.h"
#include "cajeta/asn/Scope.h"
#include "cajeta/type/Method.h"
#include "cajeta/asn/Block.h"
#include "cajeta/type/Modifiable.h"
#include "cajeta/type/Annotatable.h"
#include "cajeta/asn/CompilationUnit.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/Interface.h"
#include "cajeta/type/CajetaEnum.h"
#include "cajeta/asn/FormalParameter.h"
#include "cajeta/asn/CompilationUnit.h"
#include "cajeta/compile/CompileContext.h"
#include "cajeta/asn/PackageDeclaration.h"
#include "cajeta/asn/ImportDeclaration.h"
#include "cajeta/asn/TypeDeclaration.h"

using namespace std;

namespace cajeta {
    class CajetaParserIRListener : public CajetaParserBaseListener, public CompileContext {
    private:
        set<QualifiedName*> curAnnotations;
        set<Modifier> modifiers;
        CajetaStructure* curStructure;
        Method* curMethod;

        // Parsing state

    public:
        CajetaParserIRListener(llvm::LLVMContext* llvmContext,
                               string sourcePath,
                               string sourceRoot,
                               string archiveRoot,
                               string targetTriple,
                               llvm::TargetMachine* targetMachine) :
                CompileContext(llvmContext,
                        std::move(sourcePath),
                        std::move(sourceRoot),
                        std::move(archiveRoot),
                        std::move(targetTriple),
                        targetMachine) {
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


        virtual void enterCompilationUnit(CajetaParser::CompilationUnitContext* parseContext) override {
            compilationUnit = CompilationUnit::parse(parseContext, this);
            nodes.push(compilationUnit);

        }

        virtual void exitCompilationUnit(CajetaParser::CompilationUnitContext* parseContext) override {
            nodes.pop();
        }

        virtual void enterPackageDeclaration(CajetaParser::PackageDeclarationContext* parseContext) override {
            auto node = PackageDeclaration::parse(parseContext, this);
            nodes.front()->addChild(node);
            nodes.push(node);
        }

        virtual void exitPackageDeclaration(CajetaParser::PackageDeclarationContext* parseContext) override {
            nodes.pop();
        }

        virtual void enterImportDeclaration(CajetaParser::ImportDeclarationContext* parseContext) override {
            auto node = ImportDeclaration::parse(parseContext, this);
            nodes.front()->addChild(node);
            nodes.push(node);
        }

        virtual void exitImportDeclaration(CajetaParser::ImportDeclarationContext* parseContext) override {
            nodes.pop();
        }

        virtual void enterTypeDeclaration(CajetaParser::TypeDeclarationContext* parseContext) override {
            auto node = TypeDeclaration::parse(parseContext, this);
            nodes.front()->addChild(node);
            nodes.push(node);
        }

        virtual void exitTypeDeclaration(CajetaParser::TypeDeclarationContext *ctx) override {
            nodes.pop();
        }

        virtual void enterModifier(CajetaParser::ModifierContext* parseContext) override {
            cout << "enterModifier" << "\n";
        }
        virtual void exitModifier(CajetaParser::ModifierContext* parseContext) override {
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
        virtual void exitClassOrInterfaceModifier(CajetaParser::ClassOrInterfaceModifierContext* parseContext) override {
            cout << "exitClassOrInterfaceModifier" << "\n";
        }

        virtual void enterVariableModifier(CajetaParser::VariableModifierContext* parseContext) override {
            cout << "/*" << "\n";
        }
        virtual void exitVariableModifier(CajetaParser::VariableModifierContext* parseContext) override {
            cout << "exitVariableModifier" << "\n";
        }

        // TODO: Make this a member variable (classDefinition), so that we can add fields to the type
        virtual void enterClassDeclaration(CajetaParser::ClassDeclarationContext* ctxClassDecl) override {
            cout << "enterClassDeclaration" << "\n";
            QualifiedName* qName = QualifiedName::create(ctxClassDecl->identifier()->getText(),
                                                         compilationUnit->getQName()->getPackageName());

            curStructure = new CajetaClass(llvmContext, qName, modifiers);
            types.push_back(curStructure);
            typeStack.push(curStructure);
            modifiers.clear();
        }

        virtual void exitClassDeclaration(CajetaParser::ClassDeclarationContext* parseContext) override {
            cout << "exitClassDeclaration" << "\n";
            for (auto methodEntry : curStructure->getMethods()) {
                methodEntry.second->generate(this);
            }

        }

        virtual void enterTypeParameters(CajetaParser::TypeParametersContext* parseContext) override {
            cout << "enterTypeParameters" << "\n";
        }
        virtual void exitTypeParameters(CajetaParser::TypeParametersContext* parseContext) override {
            cout << "exitTypeParameters" << "\n";
        }

        virtual void enterTypeParameter(CajetaParser::TypeParameterContext* parseContext) override {
            cout << "enterTypeParameter" << "\n";
        }
        virtual void exitTypeParameter(CajetaParser::TypeParameterContext* parseContext) override {
            cout << "exitTypeParameter" << "\n";
        }

        virtual void enterTypeBound(CajetaParser::TypeBoundContext* parseContext) override {
            cout << "enterTypeBound" << "\n";
        }
        virtual void exitTypeBound(CajetaParser::TypeBoundContext* parseContext) override {
            cout << "exitTypeBound" << "\n";
        }

        virtual void enterEnumDeclaration(CajetaParser::EnumDeclarationContext* parseContext) override {
            cout << "enterEnumDeclaration" << "\n";
        }
        virtual void exitEnumDeclaration(CajetaParser::EnumDeclarationContext* parseContext) override {
            cout << "exitEnumDeclaration" << "\n";
        }

        virtual void enterEnumConstants(CajetaParser::EnumConstantsContext* parseContext) override {
            cout << "enterEnumConstants" << "\n";
        }
        virtual void exitEnumConstants(CajetaParser::EnumConstantsContext* parseContext) override {
            cout << "exitEnumConstants" << "\n";
        }

        virtual void enterEnumConstant(CajetaParser::EnumConstantContext* parseContext) override {
            cout << "enterEnumConstant" << "\n";
        }
        virtual void exitEnumConstant(CajetaParser::EnumConstantContext* parseContext) override {
            cout << "exitEnumConstant" << "\n";
        }

        virtual void enterEnumBodyDeclarations(CajetaParser::EnumBodyDeclarationsContext* parseContext) override {
            cout << "enterEnumBodyDeclarations" << "\n";
        }
        virtual void exitEnumBodyDeclarations(CajetaParser::EnumBodyDeclarationsContext* parseContext) override {
            cout << "exitEnumBodyDeclarations" << "\n";
        }

        virtual void enterInterfaceDeclaration(CajetaParser::InterfaceDeclarationContext* parseContext) override {
            cout << "enterInterfaceDeclaration" << "\n";
        }
        virtual void exitInterfaceDeclaration(CajetaParser::InterfaceDeclarationContext* parseContext) override {
            cout << "exitInterfaceDeclaration" << "\n";
        }

        virtual void enterClassBody(CajetaParser::ClassBodyContext* parseContext) override {
            cout << "enterClassBody" << "\n";
        }
        virtual void exitClassBody(CajetaParser::ClassBodyContext* parseContext) override {
            cout << "exitClassBody" << "\n";
        }

        virtual void enterInterfaceBody(CajetaParser::InterfaceBodyContext* parseContext) override {
            cout << "enterInterfaceBody" << "\n";
        }
        virtual void exitInterfaceBody(CajetaParser::InterfaceBodyContext* parseContext) override {
            cout << "exitInterfaceBody" << "\n";
        }

        virtual void enterClassBodyDeclaration(CajetaParser::ClassBodyDeclarationContext* parseContext) override {
            cout << "enterClassBodyDeclaration" << "\n";
        }
        virtual void exitClassBodyDeclaration(CajetaParser::ClassBodyDeclarationContext* parseContext) override {
            cout << "exitClassBodyDeclaration" << "\n";
        }

        virtual void enterMemberDeclaration(CajetaParser::MemberDeclarationContext* ctx) override {
            cout << "enterMemberDeclaration" << "\n";
        }
        virtual void exitMemberDeclaration(CajetaParser::MemberDeclarationContext* parseContext) override {
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

            //curMethod->
        }

        /**
         *
         * @param ctxExpression
         */
        void processExpression(CajetaParser::ExpressionContext* ctxExpression) {
            cout << "Entered processExpression" << "\n";
        }
        virtual void exitMethodDeclaration(CajetaParser::MethodDeclarationContext* parseContext) override {
            cout << "exitMethodDeclaration" << "\n";
        }

        virtual void enterMethodBody(CajetaParser::MethodBodyContext* parseContext) override {
            cout << "enterMethodBody" << "\n";
        }
        virtual void exitMethodBody(CajetaParser::MethodBodyContext* parseContext) override {
            cout << "exitMethodBody" << "\n";
        }

        virtual void enterTypeTypeOrVoid(CajetaParser::TypeTypeOrVoidContext* parseContext) override {
            cout << "enterTypeTypeOrVoid" << "\n";
        }
        virtual void exitTypeTypeOrVoid(CajetaParser::TypeTypeOrVoidContext* parseContext) override {
            cout << "exitTypeTypeOrVoid" << "\n";
        }

        virtual void enterGenericMethodDeclaration(CajetaParser::GenericMethodDeclarationContext* parseContext) override {
            cout << "enterGenericMethodDeclaration" << "\n";
        }
        virtual void exitGenericMethodDeclaration(CajetaParser::GenericMethodDeclarationContext* parseContext) override {
            cout << "exitGenericMethodDeclaration" << "\n";
        }

        virtual void enterGenericConstructorDeclaration(CajetaParser::GenericConstructorDeclarationContext* parseContext) override {
            cout << "enterGenericConstructorDeclaration" << "\n";
        }
        virtual void exitGenericConstructorDeclaration(CajetaParser::GenericConstructorDeclarationContext* parseContext) override {
            cout << "exitGenericConstructorDeclaration" << "\n";
        }

        virtual void enterConstructorDeclaration(CajetaParser::ConstructorDeclarationContext* parseContext) override {
            cout << "enterConstructorDeclaration" << "\n";
        }
        virtual void exitConstructorDeclaration(CajetaParser::ConstructorDeclarationContext* parseContext) override {
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
        virtual void exitFieldDeclaration(CajetaParser::FieldDeclarationContext* parseContext) override {
            cout << "exitFieldDeclaration" << "\n";
        }

        virtual void enterInterfaceBodyDeclaration(CajetaParser::InterfaceBodyDeclarationContext* parseContext) override {
            cout << "enterInterfaceBodyDeclaration" << "\n";
        }
        virtual void exitInterfaceBodyDeclaration(CajetaParser::InterfaceBodyDeclarationContext* parseContext) override {
            cout << "exitInterfaceBodyDeclaration" << "\n";
        }

        virtual void enterInterfaceMemberDeclaration(CajetaParser::InterfaceMemberDeclarationContext* parseContext) override {
            cout << "enterInterfaceMemberDeclaration" << "\n";
        }
        virtual void exitInterfaceMemberDeclaration(CajetaParser::InterfaceMemberDeclarationContext* parseContext) override {
            cout << "exitInterfaceMemberDeclaration" << "\n";
        }

        virtual void enterConstDeclaration(CajetaParser::ConstDeclarationContext* parseContext) override {
            cout << "enterConstDeclaration" << "\n";
        }
        virtual void exitConstDeclaration(CajetaParser::ConstDeclarationContext* parseContext) override {
            cout << "exitConstDeclaration" << "\n";
        }

        virtual void enterConstantDeclarator(CajetaParser::ConstantDeclaratorContext* parseContext) override {
            cout << "enterConstantDeclarator" << "\n";
        }
        virtual void exitConstantDeclarator(CajetaParser::ConstantDeclaratorContext* parseContext) override {
            cout << "exitConstantDeclarator" << "\n";
        }

        virtual void enterInterfaceMethodDeclaration(CajetaParser::InterfaceMethodDeclarationContext* parseContext) override {
            cout << "enterInterfaceMethodDeclaration" << "\n";
        }
        virtual void exitInterfaceMethodDeclaration(CajetaParser::InterfaceMethodDeclarationContext* parseContext) override {
            cout << "exitInterfaceMethodDeclaration" << "\n";
        }

        virtual void enterInterfaceMethodModifier(CajetaParser::InterfaceMethodModifierContext* parseContext) override {
            cout << "enterInterfaceMethodModifier" << "\n";
        }
        virtual void exitInterfaceMethodModifier(CajetaParser::InterfaceMethodModifierContext* parseContext) override {
            cout << "exitInterfaceMethodModifier" << "\n";
        }

        virtual void enterGenericInterfaceMethodDeclaration(CajetaParser::GenericInterfaceMethodDeclarationContext* parseContext) override {
            cout << "enterGenericInterfaceMethodDeclaration" << "\n";
        }
        virtual void exitGenericInterfaceMethodDeclaration(CajetaParser::GenericInterfaceMethodDeclarationContext* parseContext) override {
            cout << "exitGenericInterfaceMethodDeclaration" << "\n";
        }

        virtual void enterInterfaceCommonBodyDeclaration(CajetaParser::InterfaceCommonBodyDeclarationContext* parseContext) override {
            cout << "enterInterfaceCommonBodyDeclaration" << "\n";
        }
        virtual void exitInterfaceCommonBodyDeclaration(CajetaParser::InterfaceCommonBodyDeclarationContext* parseContext) override {
            cout << "exitInterfaceCommonBodyDeclaration" << "\n";
        }

        virtual void enterVariableDeclarators(CajetaParser::VariableDeclaratorsContext* parseContext) override {
            cout << "enterVariableDeclarators" << "\n";
        }
        virtual void exitVariableDeclarators(CajetaParser::VariableDeclaratorsContext* parseContext) override {
            cout << "exitVariableDeclarators" << "\n";
        }

        virtual void enterVariableDeclarator(CajetaParser::VariableDeclaratorContext* parseContext) override {
            cout << "enterVariableDeclarator" << "\n";
        }
        virtual void exitVariableDeclarator(CajetaParser::VariableDeclaratorContext* parseContext) override {
            cout << "exitVariableDeclarator" << "\n";
        }

        virtual void enterVariableDeclaratorId(CajetaParser::VariableDeclaratorIdContext* parseContext) override {
            cout << "enterVariableDeclaratorId" << "\n";
        }
        virtual void exitVariableDeclaratorId(CajetaParser::VariableDeclaratorIdContext* parseContext) override {
            cout << "exitVariableDeclaratorId" << "\n";
        }

        virtual void enterVariableInitializer(CajetaParser::VariableInitializerContext* parseContext) override {
            cout << "enterVariableInitializer" << "\n";
        }
        virtual void exitVariableInitializer(CajetaParser::VariableInitializerContext* parseContext) override {
            cout << "exitVariableInitializer" << "\n";
        }

        virtual void enterArrayInitializer(CajetaParser::ArrayInitializerContext* parseContext) override {
            cout << "enterArrayInitializer" << "\n";
        }
        virtual void exitArrayInitializer(CajetaParser::ArrayInitializerContext* parseContext) override {
            cout << "exitArrayInitializer" << "\n";
        }

        virtual void enterClassOrInterfaceType(CajetaParser::ClassOrInterfaceTypeContext* parseContext) override {
            cout << "enterClassOrInterfaceType" << "\n";
        }
        virtual void exitClassOrInterfaceType(CajetaParser::ClassOrInterfaceTypeContext* parseContext) override {
            cout << "exitClassOrInterfaceType" << "\n";
        }

        virtual void enterTypeArgument(CajetaParser::TypeArgumentContext* parseContext) override {
            cout << "enterTypeArgument" << "\n";
        }
        virtual void exitTypeArgument(CajetaParser::TypeArgumentContext* parseContext) override {
            cout << "exitTypeArgument" << "\n";
        }

        virtual void enterQualifiedNameList(CajetaParser::QualifiedNameListContext* parseContext) override {
            cout << "enterQualifiedNameList" << "\n";
        }
        virtual void exitQualifiedNameList(CajetaParser::QualifiedNameListContext* parseContext) override {
            cout << "exitQualifiedNameList" << "\n";
        }

        virtual void enterFormalParameters(CajetaParser::FormalParametersContext* parseContext) override {
            cout << "enterFormalParameters" << "\n";
        }
        virtual void exitFormalParameters(CajetaParser::FormalParametersContext* parseContext) override {
            cout << "exitFormalParameters" << "\n";
        }

        virtual void enterReceiverParameter(CajetaParser::ReceiverParameterContext* parseContext) override {
            cout << "enterReceiverParameter" << "\n";
        }
        virtual void exitReceiverParameter(CajetaParser::ReceiverParameterContext* parseContext) override {
            cout << "exitReceiverParameter" << "\n";
        }

        virtual void enterFormalParameterList(CajetaParser::FormalParameterListContext* ctx) override {
            cout << "enterFormalParameterList" << "\n";

        }
        virtual void exitFormalParameterList(CajetaParser::FormalParameterListContext* parseContext) override {
            cout << "exitFormalParameterList" << "\n";
        }

        virtual void enterFormalParameter(CajetaParser::FormalParameterContext* parseContext) override {
            cout << "enterFormalParameter" << "\n";
        }
        virtual void exitFormalParameter(CajetaParser::FormalParameterContext* parseContext) override {
            cout << "exitFormalParameter" << "\n";
        }

        virtual void enterLastFormalParameter(CajetaParser::LastFormalParameterContext* parseContext) override {
            cout << "enterLastFormalParameter" << "\n";
        }
        virtual void exitLastFormalParameter(CajetaParser::LastFormalParameterContext* parseContext) override {
            cout << "exitLastFormalParameter" << "\n";
        }

        virtual void enterLambdaLVTIList(CajetaParser::LambdaLVTIListContext* parseContext) override {
            cout << "enterLambdaLVTIList" << "\n";
        }
        virtual void exitLambdaLVTIList(CajetaParser::LambdaLVTIListContext* parseContext) override {
            cout << "exitLambdaLVTIList" << "\n";
        }

        virtual void enterLambdaLVTIParameter(CajetaParser::LambdaLVTIParameterContext* parseContext) override {
            cout << "enterLambdaLVTIParameter" << "\n";
        }
        virtual void exitLambdaLVTIParameter(CajetaParser::LambdaLVTIParameterContext* parseContext) override {
            cout << "exitLambdaLVTIParameter" << "\n";
        }

        virtual void enterQualifiedName(CajetaParser::QualifiedNameContext* parseContext) override {
            cout << "enterQualifiedName" << "\n";
        }
        virtual void exitQualifiedName(CajetaParser::QualifiedNameContext* parseContext) override {
            cout << "exitQualifiedName" << "\n";
        }

        virtual void enterLiteral(CajetaParser::LiteralContext* parseContext) override {
            cout << "enterLiteral" << "\n";
        }
        virtual void exitLiteral(CajetaParser::LiteralContext* parseContext) override {
            cout << "exitLiteral" << "\n";
        }

        virtual void enterIntegerLiteral(CajetaParser::IntegerLiteralContext* parseContext) override {
            cout << "enterIntegerLiteral" << "\n";
        }
        virtual void exitIntegerLiteral(CajetaParser::IntegerLiteralContext* parseContext) override {
            cout << "exitIntegerLiteral" << "\n";
        }

        virtual void enterFloatLiteral(CajetaParser::FloatLiteralContext* parseContext) override {
            cout << "enterFloatLiteral" << "\n";
        }
        virtual void exitFloatLiteral(CajetaParser::FloatLiteralContext* parseContext) override {
            cout << "exitFloatLiteral" << "\n";
        }

        virtual void enterAltAnnotationQualifiedName(CajetaParser::AltAnnotationQualifiedNameContext* parseContext) override {
            cout << "enterAltAnnotationQualifiedName" << "\n";
        }
        virtual void exitAltAnnotationQualifiedName(CajetaParser::AltAnnotationQualifiedNameContext* parseContext) override {
            cout << "exitAltAnnotationQualifiedName" << "\n";
        }

        virtual void enterAnnotation(CajetaParser::AnnotationContext* parseContext) override {
            cout << "enterAnnotation" << "\n";
        }
        virtual void exitAnnotation(CajetaParser::AnnotationContext* parseContext) override {
            cout << "exitAnnotation" << "\n";
        }

        virtual void enterElementValuePairs(CajetaParser::ElementValuePairsContext* parseContext) override {
            cout << "enterElementValuePairs" << "\n";
        }
        virtual void exitElementValuePairs(CajetaParser::ElementValuePairsContext* parseContext) override {
            cout << "exitElementValuePairs" << "\n";
        }

        virtual void enterElementValuePair(CajetaParser::ElementValuePairContext* parseContext) override {
            cout << "enterElementValuePair" << "\n";
        }
        virtual void exitElementValuePair(CajetaParser::ElementValuePairContext* parseContext) override {
            cout << "exitElementValuePair" << "\n";
        }

        virtual void enterElementValue(CajetaParser::ElementValueContext* parseContext) override {
            cout << "enterElementValue" << "\n";
        }
        virtual void exitElementValue(CajetaParser::ElementValueContext* parseContext) override {
            cout << "exitElementValue" << "\n";
        }

        virtual void enterElementValueArrayInitializer(CajetaParser::ElementValueArrayInitializerContext* parseContext) override {
            cout << "enterElementValueArrayInitializer" << "\n";
        }
        virtual void exitElementValueArrayInitializer(CajetaParser::ElementValueArrayInitializerContext* parseContext) override {
            cout << "exitElementValueArrayInitializer" << "\n";
        }

        virtual void enterAnnotationTypeDeclaration(CajetaParser::AnnotationTypeDeclarationContext* parseContext) override {
            cout << "enterAnnotationTypeDeclaration" << "\n";
        }
        virtual void exitAnnotationTypeDeclaration(CajetaParser::AnnotationTypeDeclarationContext* parseContext) override {
            cout << "exitAnnotationTypeDeclaration" << "\n";
        }

        virtual void enterAnnotationTypeBody(CajetaParser::AnnotationTypeBodyContext* parseContext) override {
            cout << "enterAnnotationTypeBody" << "\n";
        }
        virtual void exitAnnotationTypeBody(CajetaParser::AnnotationTypeBodyContext* parseContext) override {
            cout << "exitAnnotationTypeBody" << "\n";
        }

        virtual void enterAnnotationTypeElementDeclaration(CajetaParser::AnnotationTypeElementDeclarationContext* parseContext) override {
            cout << "enterAnnotationTypeElementDeclaration" << "\n";
        }
        virtual void exitAnnotationTypeElementDeclaration(CajetaParser::AnnotationTypeElementDeclarationContext* parseContext) override {
            cout << "exitAnnotationTypeElementDeclaration" << "\n";
        }

        virtual void enterAnnotationTypeElementRest(CajetaParser::AnnotationTypeElementRestContext* parseContext) override {
            cout << "enterAnnotationTypeElementRest" << "\n";
        }
        virtual void exitAnnotationTypeElementRest(CajetaParser::AnnotationTypeElementRestContext* parseContext) override {
            cout << "exitAnnotationTypeElementRest" << "\n";
        }

        virtual void enterAnnotationMethodOrConstantRest(CajetaParser::AnnotationMethodOrConstantRestContext* parseContext) override {
            cout << "enterAnnotationMethodOrConstantRest" << "\n";
        }
        virtual void exitAnnotationMethodOrConstantRest(CajetaParser::AnnotationMethodOrConstantRestContext* parseContext) override {
            cout << "exitAnnotationMethodOrConstantRest" << "\n";
        }

        virtual void enterAnnotationMethodRest(CajetaParser::AnnotationMethodRestContext* parseContext) override {
            cout << "enterAnnotationMethodRest" << "\n";
        }
        virtual void exitAnnotationMethodRest(CajetaParser::AnnotationMethodRestContext* parseContext) override {
            cout << "exitAnnotationMethodRest" << "\n";
        }

        virtual void enterAnnotationConstantRest(CajetaParser::AnnotationConstantRestContext* parseContext) override {
            cout << "enterAnnotationConstantRest" << "\n";
        }
        virtual void exitAnnotationConstantRest(CajetaParser::AnnotationConstantRestContext* parseContext) override {
            cout << "exitAnnotationConstantRest" << "\n";
        }

        virtual void enterDefaultValue(CajetaParser::DefaultValueContext* parseContext) override {
            cout << "enterDefaultValue" << "\n";
        }
        virtual void exitDefaultValue(CajetaParser::DefaultValueContext* parseContext) override {
            cout << "exitDefaultValue" << "\n";
        }

        virtual void enterModuleDeclaration(CajetaParser::ModuleDeclarationContext* parseContext) override {
            cout << "enterModuleDeclaration" << "\n";
        }
        virtual void exitModuleDeclaration(CajetaParser::ModuleDeclarationContext* parseContext) override {
            cout << "exitModuleDeclaration" << "\n";
        }

        virtual void enterModuleBody(CajetaParser::ModuleBodyContext* parseContext) override {
            cout << "enterModuleBody" << "\n";
        }
        virtual void exitModuleBody(CajetaParser::ModuleBodyContext* parseContext) override {
            cout << "exitModuleBody" << "\n";
        }

        virtual void enterModuleDirective(CajetaParser::ModuleDirectiveContext* parseContext) override {
            cout << "enterModuleDirective" << "\n";
        }
        virtual void exitModuleDirective(CajetaParser::ModuleDirectiveContext* parseContext) override {
            cout << "exitModuleDirective" << "\n";
        }

        virtual void enterRequiresModifier(CajetaParser::RequiresModifierContext* parseContext) override {
            cout << "enterRequiresModifier" << "\n";
        }
        virtual void exitRequiresModifier(CajetaParser::RequiresModifierContext* parseContext) override {
            cout << "exitRequiresModifier" << "\n";
        }

        virtual void enterRecordDeclaration(CajetaParser::RecordDeclarationContext* parseContext) override {
            cout << "enterRecordDeclaration" << "\n";
        }
        virtual void exitRecordDeclaration(CajetaParser::RecordDeclarationContext* parseContext) override {
            cout << "exitRecordDeclaration" << "\n";
        }

        virtual void enterRecordHeader(CajetaParser::RecordHeaderContext* parseContext) override {
            cout << "enterRecordHeader" << "\n";
        }
        virtual void exitRecordHeader(CajetaParser::RecordHeaderContext* parseContext) override {
            cout << "exitRecordHeader" << "\n";
        }

        virtual void enterRecordComponentList(CajetaParser::RecordComponentListContext* parseContext) override {
            cout << "enterRecordComponentList" << "\n";
        }
        virtual void exitRecordComponentList(CajetaParser::RecordComponentListContext* parseContext) override {
            cout << "exitRecordComponentList" << "\n";
        }

        virtual void enterRecordComponent(CajetaParser::RecordComponentContext* parseContext) override {
            cout << "enterRecordComponent" << "\n";
        }
        virtual void exitRecordComponent(CajetaParser::RecordComponentContext* parseContext) override {
            cout << "exitRecordComponent" << "\n";
        }

        virtual void enterRecordBody(CajetaParser::RecordBodyContext* parseContext) override {
            cout << "enterRecordBody" << "\n";
        }
        virtual void exitRecordBody(CajetaParser::RecordBodyContext* parseContext) override {
            cout << "exitRecordBody" << "\n";
        }

        virtual void enterBlock(CajetaParser::BlockContext* parseContext) override {
            cout << "enterBlock" << "\n";
        }
        virtual void exitBlock(CajetaParser::BlockContext* parseContext) override {
            cout << "exitBlock" << "\n";
        }

        virtual void enterBlockStatement(CajetaParser::BlockStatementContext* parseContext) override {
            cout << "enterBlockStatement" << "\n";
        }
        virtual void exitBlockStatement(CajetaParser::BlockStatementContext* parseContext) override {
            cout << "exitBlockStatement" << "\n";
        }

        virtual void enterLocalVariableDeclaration(CajetaParser::LocalVariableDeclarationContext* parseContext) override {
            cout << "enterLocalVariableDeclaration" << "\n";
        }
        virtual void exitLocalVariableDeclaration(CajetaParser::LocalVariableDeclarationContext* parseContext) override {
            cout << "exitLocalVariableDeclaration" << "\n";
        }

        virtual void enterIdentifier(CajetaParser::IdentifierContext* parseContext) override {
            cout << "enterIdentifier" << "\n";
        }
        virtual void exitIdentifier(CajetaParser::IdentifierContext* parseContext) override {
            cout << "exitIdentifier" << "\n";
        }

        virtual void enterLocalTypeDeclaration(CajetaParser::LocalTypeDeclarationContext* parseContext) override {
            cout << "enterLocalTypeDeclaration" << "\n";
        }
        virtual void exitLocalTypeDeclaration(CajetaParser::LocalTypeDeclarationContext* parseContext) override {
            cout << "exitLocalTypeDeclaration" << "\n";
        }

        virtual void enterStatement(CajetaParser::StatementContext* parseContext) override {

            cout << "enterStatement" << "\n";
        }
        virtual void exitStatement(CajetaParser::StatementContext* parseContext) override {
            cout << "exitStatement" << "\n";
        }

        virtual void enterCatchClause(CajetaParser::CatchClauseContext* parseContext) override {
            cout << "enterCatchClause" << "\n";
        }
        virtual void exitCatchClause(CajetaParser::CatchClauseContext* parseContext) override {
            cout << "exitCatchClause" << "\n";
        }

        virtual void enterCatchType(CajetaParser::CatchTypeContext* parseContext) override {
            cout << "enterCatchType" << "\n";
        }
        virtual void exitCatchType(CajetaParser::CatchTypeContext* parseContext) override {
            cout << "exitCatchType" << "\n";
        }

        virtual void enterFinallyBlock(CajetaParser::FinallyBlockContext* parseContext) override {
            cout << "enterFinallyBlock" << "\n";
        }
        virtual void exitFinallyBlock(CajetaParser::FinallyBlockContext* parseContext) override {
            cout << "exitFinallyBlock" << "\n";
        }

        virtual void enterResourceSpecification(CajetaParser::ResourceSpecificationContext* parseContext) override {
            cout << "enterResourceSpecification" << "\n";
        }
        virtual void exitResourceSpecification(CajetaParser::ResourceSpecificationContext* parseContext) override {
            cout << "exitResourceSpecification" << "\n";
        }

        virtual void enterResources(CajetaParser::ResourcesContext* parseContext) override {
            cout << "enterResources" << "\n";
        }
        virtual void exitResources(CajetaParser::ResourcesContext* parseContext) override {
            cout << "exitResources" << "\n";
        }

        virtual void enterResource(CajetaParser::ResourceContext* parseContext) override {
            cout << "enterResource" << "\n";
        }
        virtual void exitResource(CajetaParser::ResourceContext* parseContext) override {
            cout << "exitResource" << "\n";
        }

        virtual void enterSwitchBlockStatementGroup(CajetaParser::SwitchBlockStatementGroupContext* parseContext) override {
            cout << "enterSwitchBlockStatementGroup" << "\n";
        }
        virtual void exitSwitchBlockStatementGroup(CajetaParser::SwitchBlockStatementGroupContext* parseContext) override {
            cout << "exitSwitchBlockStatementGroup" << "\n";
        }

        virtual void enterSwitchLabel(CajetaParser::SwitchLabelContext* parseContext) override {
            cout << "enterSwitchLabel" << "\n";
        }
        virtual void exitSwitchLabel(CajetaParser::SwitchLabelContext* parseContext) override {
            cout << "exitSwitchLabel" << "\n";
        }

        virtual void enterForControl(CajetaParser::ForControlContext* parseContext) override {
            cout << "enterForControl" << "\n";
        }
        virtual void exitForControl(CajetaParser::ForControlContext* parseContext) override {
            cout << "exitForControl" << "\n";
        }

        virtual void enterForInit(CajetaParser::ForInitContext* parseContext) override {
            cout << "enterForInit" << "\n";
        }
        virtual void exitForInit(CajetaParser::ForInitContext* parseContext) override {
            cout << "exitForInit" << "\n";
        }

        virtual void enterEnhancedForControl(CajetaParser::EnhancedForControlContext* parseContext) override {
            cout << "enterEnhancedForControl" << "\n";
        }
        virtual void exitEnhancedForControl(CajetaParser::EnhancedForControlContext* parseContext) override {
            cout << "exitEnhancedForControl" << "\n";
        }

        virtual void enterParExpression(CajetaParser::ParExpressionContext* parseContext) override {
            cout << "enterParExpression" << "\n";
        }
        virtual void exitParExpression(CajetaParser::ParExpressionContext* parseContext) override {
            cout << "exitParExpression" << "\n";
        }

        virtual void enterExpressionList(CajetaParser::ExpressionListContext* ctx) override {
            cout << "enterExpressionList" << "\n";
            cout << "enterMethodCall" << ctx->getText() << "\n";
        }
        virtual void exitExpressionList(CajetaParser::ExpressionListContext* parseContext) override {
            cout << "exitExpressionList" << "\n";
        }

        virtual void enterMethodCall(CajetaParser::MethodCallContext* ctx) override {
            string str = ctx->getText();
            cout << "enterMethodCall" << ctx->getText() << "\n";
        }
        virtual void exitMethodCall(CajetaParser::MethodCallContext* parseContext) override {
            cout << "exitMethodCall" << "\n";
        }

        virtual void enterExpression(CajetaParser::ExpressionContext* ctx) override {
            cout << "enterExpression" << ctx->getText() << "\n";
        }
        virtual void exitExpression(CajetaParser::ExpressionContext* parseContext) override {
            cout << "exitExpression" << "\n";
        }

        virtual void enterPattern(CajetaParser::PatternContext* parseContext) override {
            cout << "enterPattern" << "\n";
        }
        virtual void exitPattern(CajetaParser::PatternContext* parseContext) override {
            cout << "exitPattern" << "\n";
        }

        virtual void enterLambdaExpression(CajetaParser::LambdaExpressionContext* parseContext) override {
            cout << "enterLambdaExpression" << "\n";
        }
        virtual void exitLambdaExpression(CajetaParser::LambdaExpressionContext* parseContext) override {
            cout << "exitLambdaExpression" << "\n";
        }

        virtual void enterLambdaParameters(CajetaParser::LambdaParametersContext* parseContext) override {
            cout << "enterLambdaParameters" << "\n";
        }
        virtual void exitLambdaParameters(CajetaParser::LambdaParametersContext* parseContext) override {
            cout << "exitLambdaParameters" << "\n";
        }

        virtual void enterLambdaBody(CajetaParser::LambdaBodyContext* parseContext) override {
            cout << "enterLambdaBody" << "\n";
        }
        virtual void exitLambdaBody(CajetaParser::LambdaBodyContext* parseContext) override {
            cout << "exitLambdaBody" << "\n";
        }

        virtual void enterPrimary(CajetaParser::PrimaryContext* parseContext) override {
            cout << "enterPrimary" << "\n";
        }
        virtual void exitPrimary(CajetaParser::PrimaryContext* parseContext) override {
            cout << "exitPrimary" << "\n";
        }

        virtual void enterSwitchExpression(CajetaParser::SwitchExpressionContext* parseContext) override {
            cout << "enterSwitchExpression" << "\n";
        }
        virtual void exitSwitchExpression(CajetaParser::SwitchExpressionContext* parseContext) override {
            cout << "exitSwitchExpression" << "\n";
        }

        virtual void enterSwitchLabeledRule(CajetaParser::SwitchLabeledRuleContext* parseContext) override {
            cout << "enterSwitchLabeledRule" << "\n";
        }
        virtual void exitSwitchLabeledRule(CajetaParser::SwitchLabeledRuleContext* parseContext) override {
            cout << "exitSwitchLabeledRule" << "\n";
        }

        virtual void enterGuardedPattern(CajetaParser::GuardedPatternContext* parseContext) override {
            cout << "enterGuardedPattern" << "\n";
        }
        virtual void exitGuardedPattern(CajetaParser::GuardedPatternContext* parseContext) override {
            cout << "exitGuardedPattern" << "\n";
        }

        virtual void enterSwitchRuleOutcome(CajetaParser::SwitchRuleOutcomeContext* parseContext) override {
            cout << "enterSwitchRuleOutcome" << "\n";
        }
        virtual void exitSwitchRuleOutcome(CajetaParser::SwitchRuleOutcomeContext* parseContext) override {
            cout << "exitSwitchRuleOutcome" << "\n";
        }

        virtual void enterClassType(CajetaParser::ClassTypeContext* parseContext) override {
            cout << "enterClassType" << "\n";
        }
        virtual void exitClassType(CajetaParser::ClassTypeContext* parseContext) override {
            cout << "exitClassType" << "\n";
        }

        virtual void enterCreator(CajetaParser::CreatorContext* parseContext) override {
            cout << "enterCreator" << "\n";
        }
        virtual void exitCreator(CajetaParser::CreatorContext* parseContext) override {
            cout << "exitCreator" << "\n";
        }

        virtual void enterCreatedName(CajetaParser::CreatedNameContext* parseContext) override {
            cout << "enterCreatedName" << "\n";
        }
        virtual void exitCreatedName(CajetaParser::CreatedNameContext* parseContext) override {
            cout << "exitCreatedName" << "\n";
        }

        virtual void enterInnerCreator(CajetaParser::InnerCreatorContext* parseContext) override {
            cout << "enterInnerCreator" << "\n";
        }
        virtual void exitInnerCreator(CajetaParser::InnerCreatorContext* parseContext) override {
            cout << "exitInnerCreator" << "\n";
        }

        virtual void enterArrayCreatorRest(CajetaParser::ArrayCreatorRestContext* parseContext) override {
            cout << "enterArrayCreatorRest" << "\n";
        }
        virtual void exitArrayCreatorRest(CajetaParser::ArrayCreatorRestContext* parseContext) override {
            cout << "exitArrayCreatorRest" << "\n";
        }

        virtual void enterClassCreatorRest(CajetaParser::ClassCreatorRestContext* parseContext) override {
            cout << "enterClassCreatorRest" << "\n";
        }
        virtual void exitClassCreatorRest(CajetaParser::ClassCreatorRestContext* parseContext) override {
            cout << "exitClassCreatorRest" << "\n";
        }

        virtual void enterExplicitGenericInvocation(CajetaParser::ExplicitGenericInvocationContext* parseContext) override {
            cout << "enterExplicitGenericInvocation" << "\n";
        }
        virtual void exitExplicitGenericInvocation(CajetaParser::ExplicitGenericInvocationContext* parseContext) override {
            cout << "exitExplicitGenericInvocation" << "\n";
        }

        virtual void enterTypeArgumentsOrDiamond(CajetaParser::TypeArgumentsOrDiamondContext* parseContext) override {
            cout << "enterTypeArgumentsOrDiamond" << "\n";
        }
        virtual void exitTypeArgumentsOrDiamond(CajetaParser::TypeArgumentsOrDiamondContext* parseContext) override {
            cout << "exitTypeArgumentsOrDiamond" << "\n";
        }

        virtual void enterNonWildcardTypeArgumentsOrDiamond(CajetaParser::NonWildcardTypeArgumentsOrDiamondContext* parseContext) override {
            cout << "enterNonWildcardTypeArgumentsOrDiamond" << "\n";
        }
        virtual void exitNonWildcardTypeArgumentsOrDiamond(CajetaParser::NonWildcardTypeArgumentsOrDiamondContext* parseContext) override {
            cout << "exitNonWildcardTypeArgumentsOrDiamond" << "\n";
        }

        virtual void enterNonWildcardTypeArguments(CajetaParser::NonWildcardTypeArgumentsContext* parseContext) override {
            cout << "enterNonWildcardTypeArguments" << "\n";
        }
        virtual void exitNonWildcardTypeArguments(CajetaParser::NonWildcardTypeArgumentsContext* parseContext) override {
            cout << "exitNonWildcardTypeArguments" << "\n";
        }

        virtual void enterTypeList(CajetaParser::TypeListContext* parseContext) override {
            cout << "enterTypeList" << "\n";
        }
        virtual void exitTypeList(CajetaParser::TypeListContext* parseContext) override {
            cout << "exitTypeList" << "\n";
        }

        virtual void enterTypeType(CajetaParser::TypeTypeContext* parseContext) override {
            cout << "enterTypeType" << "\n";
        }
        virtual void exitTypeType(CajetaParser::TypeTypeContext* parseContext) override {
            cout << "exitTypeType" << "\n";
        }

        virtual void enterPrimitiveType(CajetaParser::PrimitiveTypeContext* parseContext) override {

            cout << "enterPrimitiveType" << "\n";
        }
        virtual void exitPrimitiveType(CajetaParser::PrimitiveTypeContext* parseContext) override {
            cout << "exitPrimitiveType" << "\n";
        }

        virtual void enterTypeArguments(CajetaParser::TypeArgumentsContext* parseContext) override {
            cout << "enterTypeArguments" << "\n";
        }
        virtual void exitTypeArguments(CajetaParser::TypeArgumentsContext* parseContext) override {
            cout << "exitTypeArguments" << "\n";
        }

        virtual void enterSuperSuffix(CajetaParser::SuperSuffixContext* parseContext) override {
            cout << "enterSuperSuffix" << "\n";
        }
        virtual void exitSuperSuffix(CajetaParser::SuperSuffixContext* parseContext) override {
            cout << "exitSuperSuffix" << "\n";
        }

        virtual void enterExplicitGenericInvocationSuffix(CajetaParser::ExplicitGenericInvocationSuffixContext* parseContext) override {
            cout << "enterExplicitGenericInvocationSuffix" << "\n";
        }
        virtual void exitExplicitGenericInvocationSuffix(CajetaParser::ExplicitGenericInvocationSuffixContext* parseContext) override {
            cout << "exitExplicitGenericInvocationSuffix" << "\n";
        }

        virtual void enterArguments(CajetaParser::ArgumentsContext* parseContext) override {
            cout << "enterArguments" << "\n";
        }
        virtual void exitArguments(CajetaParser::ArgumentsContext* parseContext) override {
            cout << "exitArguments" << "\n";
        }

        virtual void enterEveryRule(antlr4::ParserRuleContext* parseContext) override {
            cout << "void" << "\n";
        }
        virtual void exitEveryRule(antlr4::ParserRuleContext* parseContext) override {
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