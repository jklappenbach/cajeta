//
// Created by James Klappenbach on 2/8/22.
//

#ifndef CAJETA_CAJETAPARSERIRLISTENER_H
#define CAJETA_CAJETAPARSERIRLISTENER_H

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <CajetaParserBaseVisitor.h>
#include <CajetaParser.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <CajetaParserBaseListener.h>
#include <llvm/ADT/StringRef.h>

using namespace std;

namespace cajeta {
    enum ParseState { DEFINE_CLASS, DEFINE_VARIABLE, DEFINE_METHOD_SIG, DEFINE_METHOD };
    enum AccessModifier { PACKAGE = 0x01, PUBLIC = 0x02, PRIVATE = 0x04, PROTECTED = 0x08, STATIC = 0x0F, FINAL = 0x10 };

    AccessModifier toAccessModifier(string value);

    struct TypeParameter {
        string parameterName;
        TypeParameter(string parameterName) {
            this->parameterName = parameterName;
        }
    };

    // TODO: Fix me!
//    struct ClassAnnotation {
//        void onField(string fieldName, Type* type, Structure);
//    };

    struct FieldAnnotation {

    };

    struct MethodAnnotation {

    };

    struct ParameterAnnotation {

    };

    struct ExtendedTypeParameter : TypeParameter {
        string extends;
        llvm::Type* type;
        ExtendedTypeParameter(string parameterName, string extends) : TypeParameter(parameterName) {
            this->extends = extends;
        }
    };

    struct TypeDefinition;

    struct ParseContext {
        llvm::LLVMContext* llvmContext;
        llvm::Module* module;
        llvm::IRBuilder<>* builder;
        map<string, TypeDefinition*> types;

        ParseContext(llvm::LLVMContext* llvmContext,
                     llvm::Module* module,
                     llvm::IRBuilder<>* builder) {
            this->llvmContext = llvmContext;
            this->module = module;
            this->builder = builder;
        }
    };

    struct TypeDefinition {
        string name;
        llvm::Module* module;
        llvm::Type* type;
        bool reference;

        virtual void define() = 0;

        virtual void allocate() = 0;

        virtual void release() = 0;

        TypeDefinition() {
            reference = false;
        }

        TypeDefinition(string name, llvm::Module* module, llvm::Type* type, bool reference) {
            this->name = name;
            this->module = module;
            this->type = type;
            this->reference = reference;
        }

        static TypeDefinition* fromName(string name, ParseContext* ctxParse);
        static TypeDefinition* fromContext(CajetaParser::TypeTypeOrVoidContext* ctxTypeType, ParseContext* ctxParse);
    };

    struct NativeTypeDefinition : TypeDefinition {
        NativeTypeDefinition(string name, llvm::Module* module, llvm::Type* type) :
                NativeTypeDefinition(name, module, type, false) {
        }
        NativeTypeDefinition(string name, llvm::Module* module, llvm::Type* type, bool reference) {
            this->name = name;
            this->module = module;
            this->type = type;
            this->reference = reference;
        }

        virtual void define() {

        }

        virtual void allocate() {

        }

        virtual void release() {

        }

        static TypeDefinition* fromName(string name, ParseContext* ctxParse);
    };

    struct Field {
        bool reference;
        string name;
        string typeName;
        llvm::AllocaInst* definition;
        llvm::Type* type;
        void define() { }
        void allocate(llvm::IRBuilder<>* builder, llvm::Module* module, llvm::LLVMContext* llvmContext) {
            // typeDefinition->allocate(builder, module, llvmContext);
        }
        void release(llvm::IRBuilder<>* builder, llvm::Module* module, llvm::LLVMContext* llvmContext) {
            // typeDefinition->free(builder, module, llvmContext);
        }
    };

    enum ScopeType { MODULE_SCOPE, CLASS_SCOPE, METHOD_SCOPE, BLOCK_SCOPE };

    struct Scope {
        ScopeType scopeType;
        Scope* parent;
        map<string, Field*> fields;
        Scope() {
            parent = NULL;
            scopeType = CLASS_SCOPE;
        }
        Scope(Scope* parent, ScopeType scopeType) {
            this->parent = parent;
            this->scopeType = scopeType;
        }


        ~Scope() {
            for (auto itr = fields.begin(); itr != fields.end(); itr++) {
                Field* field = itr->second;
            }
            fields.clear();
        }

        Field* findField(string fieldName) {
            Field* field = fields[fieldName];
            if (field == NULL && parent != NULL) {
                return parent->findField(fieldName);
            }
            return field;
        }
    };

    struct StructureDefinition : TypeDefinition {
        list<TypeParameter*> typeParameters;
        llvm::StructType* structType;
        int accessModifiers;
        string name;
        string package;
        list<string> typeList;  // Inheritance chain
        map<string, list<pair<string, llvm::Type*>>> methods;
        std::map<std::string, Field*> fields;
        llvm::Module* module;

        StructureDefinition() {
            accessModifiers = 0;
        }
        string getCanonicalName() {
            return package + "." + name;
        }
    };

    struct ClassDefinition : StructureDefinition {
        Scope scope;
        void createBody() {
            // TODO build me!
        }

        virtual void define() { }
        virtual void allocate() { }
        virtual void release() { }
    };

    struct EnumDefinition : StructureDefinition {
        list<string> constants; // Enum specific,
        void createBody() {
            // TODO build me!
        }
    };

    struct InterfaceDefinition : StructureDefinition {
        void createBody() {
            // TODO build me!
        }
    };

    struct RecordDefinition : StructureDefinition {
        void createBody() {
            // TODO build me!
        }
    };

    class CajetaParserIRListener : public CajetaParserBaseListener, ParseContext {
    private:
        // Parsing state
        string srcPath;
        string targetPath;
        ParseState parseState;
        string package;
        map<string, string> memberVariables;
        map<string, list<pair<string, string>>> memberMethods;
        deque<TypeDefinition*> classStack;
        list<TypeDefinition*> classes;
        int accessModifiers;
        std::map<std::string, llvm::StructType*> allocatedClasses;
        Scope moduleScope;
        Scope* currentScope;

    public:
        CajetaParserIRListener(string srcPath,
                              llvm::LLVMContext* llvmContext,
                              string targetPath,
                              string targetTriple,
                              llvm::TargetMachine* targetMachine) :
                ParseContext(llvmContext, new llvm::Module(srcPath, *llvmContext),
                             new llvm::IRBuilder<>(*llvmContext)),
                moduleScope(NULL, MODULE_SCOPE) {
            parseState = DEFINE_CLASS;
            module->setDataLayout(targetMachine->createDataLayout());
            module->setTargetTriple(targetTriple);
            accessModifiers = 0;

            std::error_code ec;
            llvm::raw_fd_ostream dest(targetPath, ec, llvm::sys::fs::OF_None);

            //llvmContext->pImpl->

//            legacy::PassManager pass;
//            auto FileType = llvm::CGFT_ObjectFile;
//
//            if (targetMachine->addPassesToEmitFile(pass, dest, nullptr, FileType)) {
//                error = "TargetMachine can't emit a file of this type";
//                return;
//            }
//
//            pass.run(*module);
//            dest.flush();
        }


        virtual void enterCompilationUnit(CajetaParser::CompilationUnitContext * /*ctx*/) override {
            printf("Entering Compiliation Unit");
        }
        virtual void exitCompilationUnit(CajetaParser::CompilationUnitContext * /*ctx*/) override { }

        virtual void enterPackageDeclaration(CajetaParser::PackageDeclarationContext * /*ctx*/) override { }
        virtual void exitPackageDeclaration(CajetaParser::PackageDeclarationContext * /*ctx*/) override { }

        virtual void enterImportDeclaration(CajetaParser::ImportDeclarationContext * /*ctx*/) override { }
        virtual void exitImportDeclaration(CajetaParser::ImportDeclarationContext * /*ctx*/) override { }

        virtual void enterTypeDeclaration(CajetaParser::TypeDeclarationContext * /*ctx*/) override { }
        virtual void exitTypeDeclaration(CajetaParser::TypeDeclarationContext * /*ctx*/) override { }

        virtual void enterModifier(CajetaParser::ModifierContext * /*ctx*/) override { }
        virtual void exitModifier(CajetaParser::ModifierContext * /*ctx*/) override { }

        virtual void enterClassOrInterfaceModifier(CajetaParser::ClassOrInterfaceModifierContext* ctx) override {
            this->accessModifiers |= toAccessModifier(ctx->getText());
        }

        virtual void exitClassOrInterfaceModifier(CajetaParser::ClassOrInterfaceModifierContext * /*ctx*/) override { }

        virtual void enterVariableModifier(CajetaParser::VariableModifierContext * /*ctx*/) override { }
        virtual void exitVariableModifier(CajetaParser::VariableModifierContext * /*ctx*/) override { }

        virtual void enterClassDeclaration(CajetaParser::ClassDeclarationContext * /*ctx*/) override {
            ClassDefinition* classDefinition = new ClassDefinition;
            classes.push_back(classDefinition);
            classStack.push_front(classDefinition);
            currentScope = &classDefinition->scope;
            classDefinition->structType = llvm::StructType::create(*llvmContext, llvm::StringRef(classDefinition->name));
            classDefinition->accessModifiers = accessModifiers;
            accessModifiers = 0;
            // TODO Inheritance
            //structDefinition->typeParameters;

        }
        virtual void exitClassDeclaration(CajetaParser::ClassDeclarationContext * /*ctx*/) override { }

        virtual void enterTypeParameters(CajetaParser::TypeParametersContext * /*ctx*/) override { }
        virtual void exitTypeParameters(CajetaParser::TypeParametersContext * /*ctx*/) override { }

        virtual void enterTypeParameter(CajetaParser::TypeParameterContext * /*ctx*/) override { }
        virtual void exitTypeParameter(CajetaParser::TypeParameterContext * /*ctx*/) override { }

        virtual void enterTypeBound(CajetaParser::TypeBoundContext * /*ctx*/) override { }
        virtual void exitTypeBound(CajetaParser::TypeBoundContext * /*ctx*/) override { }

        virtual void enterEnumDeclaration(CajetaParser::EnumDeclarationContext * /*ctx*/) override { }
        virtual void exitEnumDeclaration(CajetaParser::EnumDeclarationContext * /*ctx*/) override { }

        virtual void enterEnumConstants(CajetaParser::EnumConstantsContext * /*ctx*/) override { }
        virtual void exitEnumConstants(CajetaParser::EnumConstantsContext * /*ctx*/) override { }

        virtual void enterEnumConstant(CajetaParser::EnumConstantContext * /*ctx*/) override { }
        virtual void exitEnumConstant(CajetaParser::EnumConstantContext * /*ctx*/) override { }

        virtual void enterEnumBodyDeclarations(CajetaParser::EnumBodyDeclarationsContext * /*ctx*/) override { }
        virtual void exitEnumBodyDeclarations(CajetaParser::EnumBodyDeclarationsContext * /*ctx*/) override { }

        virtual void enterInterfaceDeclaration(CajetaParser::InterfaceDeclarationContext * /*ctx*/) override { }
        virtual void exitInterfaceDeclaration(CajetaParser::InterfaceDeclarationContext * /*ctx*/) override { }

        virtual void enterClassBody(CajetaParser::ClassBodyContext * /*ctx*/) override { }
        virtual void exitClassBody(CajetaParser::ClassBodyContext * /*ctx*/) override { }

        virtual void enterInterfaceBody(CajetaParser::InterfaceBodyContext * /*ctx*/) override { }
        virtual void exitInterfaceBody(CajetaParser::InterfaceBodyContext * /*ctx*/) override { }

        virtual void enterClassBodyDeclaration(CajetaParser::ClassBodyDeclarationContext * /*ctx*/) override { }
        virtual void exitClassBodyDeclaration(CajetaParser::ClassBodyDeclarationContext * /*ctx*/) override { }

        virtual void enterMemberDeclaration(CajetaParser::MemberDeclarationContext * /*ctx*/) override { }
        virtual void exitMemberDeclaration(CajetaParser::MemberDeclarationContext * /*ctx*/) override { }

        virtual void enterMethodDeclaration(CajetaParser::MethodDeclarationContext *ctx) override {
            TypeDefinition* returnType = TypeDefinition::fromContext(ctx->typeTypeOrVoid(), this);

            llvm::FunctionType* functionType = llvm::FunctionType::get(returnType->type, false);
            string methodIdentifier = ctx->identifier()->getText();
            llvm::Function* function = llvm::Function::Create(functionType,
                                                              llvm::Function::ExternalLinkage,
                                                              methodIdentifier,
                                                              module);

//
            CajetaParser::MethodBodyContext* ctxMethodBody = ctx->methodBody();
            std::vector<CajetaParser::BlockStatementContext *> statements = ctxMethodBody->block()->blockStatement();
            for (auto itr = statements.begin(); itr != statements.end(); itr++) {
                CajetaParser::BlockStatementContext* ctxBlockStatement = *itr;
                string str = ctxBlockStatement->getText();
                CajetaParser::StatementContext* ctxStatement = ctxBlockStatement->statement();
                std::vector<CajetaParser::StatementContext *> statements = ctxStatement->statement();
                for (auto itrStatements = statements.begin(); itrStatements != statements.end(); itrStatements++) {
                    CajetaParser::StatementContext* ctxStatement = *itrStatements;
                    string str = ctxStatement->getText();
                }
                std::vector<CajetaParser::ExpressionContext *> expressions = ctxStatement->expression();
                for (auto itrExpressions = expressions.begin(); itrExpressions != expressions.end(); itrExpressions++) {
                    CajetaParser::ExpressionContext* ctxExpression = *itrExpressions;
                    string str = ctxExpression->getText();
                }
            }
//            std::vector<antlr4::tree::TerminalNode *> LBRACK();
//            antlr4::tree::TerminalNode* LBRACK(size_t i);
//            std::vector<antlr4::tree::TerminalNode *> RBRACK();
//            antlr4::tree::TerminalNode* RBRACK(size_t i);
//            antlr4::tree::TerminalNode *THROWS();
//            QualifiedNameListContext *qualifiedNameList();

        }

        virtual void exitMethodDeclaration(CajetaParser::MethodDeclarationContext * /*ctx*/) override { }

        virtual void enterMethodBody(CajetaParser::MethodBodyContext * /*ctx*/) override { }
        virtual void exitMethodBody(CajetaParser::MethodBodyContext * /*ctx*/) override { }

        virtual void enterTypeTypeOrVoid(CajetaParser::TypeTypeOrVoidContext * /*ctx*/) override { }
        virtual void exitTypeTypeOrVoid(CajetaParser::TypeTypeOrVoidContext * /*ctx*/) override { }

        virtual void enterGenericMethodDeclaration(CajetaParser::GenericMethodDeclarationContext * /*ctx*/) override { }
        virtual void exitGenericMethodDeclaration(CajetaParser::GenericMethodDeclarationContext * /*ctx*/) override { }

        virtual void enterGenericConstructorDeclaration(CajetaParser::GenericConstructorDeclarationContext * /*ctx*/) override { }
        virtual void exitGenericConstructorDeclaration(CajetaParser::GenericConstructorDeclarationContext * /*ctx*/) override { }

        virtual void enterConstructorDeclaration(CajetaParser::ConstructorDeclarationContext * /*ctx*/) override { }
        virtual void exitConstructorDeclaration(CajetaParser::ConstructorDeclarationContext * /*ctx*/) override { }

        virtual void enterFieldDeclaration(CajetaParser::FieldDeclarationContext * /*ctx*/) override { }
        virtual void exitFieldDeclaration(CajetaParser::FieldDeclarationContext * /*ctx*/) override { }

        virtual void enterInterfaceBodyDeclaration(CajetaParser::InterfaceBodyDeclarationContext * /*ctx*/) override { }
        virtual void exitInterfaceBodyDeclaration(CajetaParser::InterfaceBodyDeclarationContext * /*ctx*/) override { }

        virtual void enterInterfaceMemberDeclaration(CajetaParser::InterfaceMemberDeclarationContext * /*ctx*/) override { }
        virtual void exitInterfaceMemberDeclaration(CajetaParser::InterfaceMemberDeclarationContext * /*ctx*/) override { }

        virtual void enterConstDeclaration(CajetaParser::ConstDeclarationContext * /*ctx*/) override { }
        virtual void exitConstDeclaration(CajetaParser::ConstDeclarationContext * /*ctx*/) override { }

        virtual void enterConstantDeclarator(CajetaParser::ConstantDeclaratorContext * /*ctx*/) override { }
        virtual void exitConstantDeclarator(CajetaParser::ConstantDeclaratorContext * /*ctx*/) override { }

        virtual void enterInterfaceMethodDeclaration(CajetaParser::InterfaceMethodDeclarationContext * /*ctx*/) override { }
        virtual void exitInterfaceMethodDeclaration(CajetaParser::InterfaceMethodDeclarationContext * /*ctx*/) override { }

        virtual void enterInterfaceMethodModifier(CajetaParser::InterfaceMethodModifierContext * /*ctx*/) override { }
        virtual void exitInterfaceMethodModifier(CajetaParser::InterfaceMethodModifierContext * /*ctx*/) override { }

        virtual void enterGenericInterfaceMethodDeclaration(CajetaParser::GenericInterfaceMethodDeclarationContext * /*ctx*/) override { }
        virtual void exitGenericInterfaceMethodDeclaration(CajetaParser::GenericInterfaceMethodDeclarationContext * /*ctx*/) override { }

        virtual void enterInterfaceCommonBodyDeclaration(CajetaParser::InterfaceCommonBodyDeclarationContext * /*ctx*/) override { }
        virtual void exitInterfaceCommonBodyDeclaration(CajetaParser::InterfaceCommonBodyDeclarationContext * /*ctx*/) override { }

        virtual void enterVariableDeclarators(CajetaParser::VariableDeclaratorsContext * /*ctx*/) override { }
        virtual void exitVariableDeclarators(CajetaParser::VariableDeclaratorsContext * /*ctx*/) override { }

        virtual void enterVariableDeclarator(CajetaParser::VariableDeclaratorContext * /*ctx*/) override { }
        virtual void exitVariableDeclarator(CajetaParser::VariableDeclaratorContext * /*ctx*/) override { }

        virtual void enterVariableDeclaratorId(CajetaParser::VariableDeclaratorIdContext * /*ctx*/) override { }
        virtual void exitVariableDeclaratorId(CajetaParser::VariableDeclaratorIdContext * /*ctx*/) override { }

        virtual void enterVariableInitializer(CajetaParser::VariableInitializerContext * /*ctx*/) override { }
        virtual void exitVariableInitializer(CajetaParser::VariableInitializerContext * /*ctx*/) override { }

        virtual void enterArrayInitializer(CajetaParser::ArrayInitializerContext * /*ctx*/) override { }
        virtual void exitArrayInitializer(CajetaParser::ArrayInitializerContext * /*ctx*/) override { }

        virtual void enterClassOrInterfaceType(CajetaParser::ClassOrInterfaceTypeContext * /*ctx*/) override { }
        virtual void exitClassOrInterfaceType(CajetaParser::ClassOrInterfaceTypeContext * /*ctx*/) override { }

        virtual void enterTypeArgument(CajetaParser::TypeArgumentContext * /*ctx*/) override { }
        virtual void exitTypeArgument(CajetaParser::TypeArgumentContext * /*ctx*/) override { }

        virtual void enterQualifiedNameList(CajetaParser::QualifiedNameListContext * /*ctx*/) override { }
        virtual void exitQualifiedNameList(CajetaParser::QualifiedNameListContext * /*ctx*/) override { }

        virtual void enterFormalParameters(CajetaParser::FormalParametersContext * /*ctx*/) override { }
        virtual void exitFormalParameters(CajetaParser::FormalParametersContext * /*ctx*/) override { }

        virtual void enterReceiverParameter(CajetaParser::ReceiverParameterContext * /*ctx*/) override { }
        virtual void exitReceiverParameter(CajetaParser::ReceiverParameterContext * /*ctx*/) override { }

        virtual void enterFormalParameterList(CajetaParser::FormalParameterListContext * /*ctx*/) override { }
        virtual void exitFormalParameterList(CajetaParser::FormalParameterListContext * /*ctx*/) override { }

        virtual void enterFormalParameter(CajetaParser::FormalParameterContext * /*ctx*/) override { }
        virtual void exitFormalParameter(CajetaParser::FormalParameterContext * /*ctx*/) override { }

        virtual void enterLastFormalParameter(CajetaParser::LastFormalParameterContext * /*ctx*/) override { }
        virtual void exitLastFormalParameter(CajetaParser::LastFormalParameterContext * /*ctx*/) override { }

        virtual void enterLambdaLVTIList(CajetaParser::LambdaLVTIListContext * /*ctx*/) override { }
        virtual void exitLambdaLVTIList(CajetaParser::LambdaLVTIListContext * /*ctx*/) override { }

        virtual void enterLambdaLVTIParameter(CajetaParser::LambdaLVTIParameterContext * /*ctx*/) override { }
        virtual void exitLambdaLVTIParameter(CajetaParser::LambdaLVTIParameterContext * /*ctx*/) override { }

        virtual void enterQualifiedName(CajetaParser::QualifiedNameContext * /*ctx*/) override { }
        virtual void exitQualifiedName(CajetaParser::QualifiedNameContext * /*ctx*/) override { }

        virtual void enterLiteral(CajetaParser::LiteralContext * /*ctx*/) override { }
        virtual void exitLiteral(CajetaParser::LiteralContext * /*ctx*/) override { }

        virtual void enterIntegerLiteral(CajetaParser::IntegerLiteralContext * /*ctx*/) override { }
        virtual void exitIntegerLiteral(CajetaParser::IntegerLiteralContext * /*ctx*/) override { }

        virtual void enterFloatLiteral(CajetaParser::FloatLiteralContext * /*ctx*/) override { }
        virtual void exitFloatLiteral(CajetaParser::FloatLiteralContext * /*ctx*/) override { }

        virtual void enterAltAnnotationQualifiedName(CajetaParser::AltAnnotationQualifiedNameContext * /*ctx*/) override { }
        virtual void exitAltAnnotationQualifiedName(CajetaParser::AltAnnotationQualifiedNameContext * /*ctx*/) override { }

        virtual void enterAnnotation(CajetaParser::AnnotationContext * /*ctx*/) override { }
        virtual void exitAnnotation(CajetaParser::AnnotationContext * /*ctx*/) override { }

        virtual void enterElementValuePairs(CajetaParser::ElementValuePairsContext * /*ctx*/) override { }
        virtual void exitElementValuePairs(CajetaParser::ElementValuePairsContext * /*ctx*/) override { }

        virtual void enterElementValuePair(CajetaParser::ElementValuePairContext * /*ctx*/) override { }
        virtual void exitElementValuePair(CajetaParser::ElementValuePairContext * /*ctx*/) override { }

        virtual void enterElementValue(CajetaParser::ElementValueContext * /*ctx*/) override { }
        virtual void exitElementValue(CajetaParser::ElementValueContext * /*ctx*/) override { }

        virtual void enterElementValueArrayInitializer(CajetaParser::ElementValueArrayInitializerContext * /*ctx*/) override { }
        virtual void exitElementValueArrayInitializer(CajetaParser::ElementValueArrayInitializerContext * /*ctx*/) override { }

        virtual void enterAnnotationTypeDeclaration(CajetaParser::AnnotationTypeDeclarationContext * /*ctx*/) override { }
        virtual void exitAnnotationTypeDeclaration(CajetaParser::AnnotationTypeDeclarationContext * /*ctx*/) override { }

        virtual void enterAnnotationTypeBody(CajetaParser::AnnotationTypeBodyContext * /*ctx*/) override { }
        virtual void exitAnnotationTypeBody(CajetaParser::AnnotationTypeBodyContext * /*ctx*/) override { }

        virtual void enterAnnotationTypeElementDeclaration(CajetaParser::AnnotationTypeElementDeclarationContext * /*ctx*/) override { }
        virtual void exitAnnotationTypeElementDeclaration(CajetaParser::AnnotationTypeElementDeclarationContext * /*ctx*/) override { }

        virtual void enterAnnotationTypeElementRest(CajetaParser::AnnotationTypeElementRestContext * /*ctx*/) override { }
        virtual void exitAnnotationTypeElementRest(CajetaParser::AnnotationTypeElementRestContext * /*ctx*/) override { }

        virtual void enterAnnotationMethodOrConstantRest(CajetaParser::AnnotationMethodOrConstantRestContext * /*ctx*/) override { }
        virtual void exitAnnotationMethodOrConstantRest(CajetaParser::AnnotationMethodOrConstantRestContext * /*ctx*/) override { }

        virtual void enterAnnotationMethodRest(CajetaParser::AnnotationMethodRestContext * /*ctx*/) override { }
        virtual void exitAnnotationMethodRest(CajetaParser::AnnotationMethodRestContext * /*ctx*/) override { }

        virtual void enterAnnotationConstantRest(CajetaParser::AnnotationConstantRestContext * /*ctx*/) override { }
        virtual void exitAnnotationConstantRest(CajetaParser::AnnotationConstantRestContext * /*ctx*/) override { }

        virtual void enterDefaultValue(CajetaParser::DefaultValueContext * /*ctx*/) override { }
        virtual void exitDefaultValue(CajetaParser::DefaultValueContext * /*ctx*/) override { }

        virtual void enterModuleDeclaration(CajetaParser::ModuleDeclarationContext * /*ctx*/) override { }
        virtual void exitModuleDeclaration(CajetaParser::ModuleDeclarationContext * /*ctx*/) override { }

        virtual void enterModuleBody(CajetaParser::ModuleBodyContext * /*ctx*/) override { }
        virtual void exitModuleBody(CajetaParser::ModuleBodyContext * /*ctx*/) override { }

        virtual void enterModuleDirective(CajetaParser::ModuleDirectiveContext * /*ctx*/) override { }
        virtual void exitModuleDirective(CajetaParser::ModuleDirectiveContext * /*ctx*/) override { }

        virtual void enterRequiresModifier(CajetaParser::RequiresModifierContext * /*ctx*/) override { }
        virtual void exitRequiresModifier(CajetaParser::RequiresModifierContext * /*ctx*/) override { }

        virtual void enterRecordDeclaration(CajetaParser::RecordDeclarationContext * /*ctx*/) override { }
        virtual void exitRecordDeclaration(CajetaParser::RecordDeclarationContext * /*ctx*/) override { }

        virtual void enterRecordHeader(CajetaParser::RecordHeaderContext * /*ctx*/) override { }
        virtual void exitRecordHeader(CajetaParser::RecordHeaderContext * /*ctx*/) override { }

        virtual void enterRecordComponentList(CajetaParser::RecordComponentListContext * /*ctx*/) override { }
        virtual void exitRecordComponentList(CajetaParser::RecordComponentListContext * /*ctx*/) override { }

        virtual void enterRecordComponent(CajetaParser::RecordComponentContext * /*ctx*/) override { }
        virtual void exitRecordComponent(CajetaParser::RecordComponentContext * /*ctx*/) override { }

        virtual void enterRecordBody(CajetaParser::RecordBodyContext * /*ctx*/) override { }
        virtual void exitRecordBody(CajetaParser::RecordBodyContext * /*ctx*/) override { }

        virtual void enterBlock(CajetaParser::BlockContext * /*ctx*/) override { }
        virtual void exitBlock(CajetaParser::BlockContext * /*ctx*/) override { }

        virtual void enterBlockStatement(CajetaParser::BlockStatementContext * /*ctx*/) override { }
        virtual void exitBlockStatement(CajetaParser::BlockStatementContext * /*ctx*/) override { }

        virtual void enterLocalVariableDeclaration(CajetaParser::LocalVariableDeclarationContext * /*ctx*/) override { }
        virtual void exitLocalVariableDeclaration(CajetaParser::LocalVariableDeclarationContext * /*ctx*/) override { }

        virtual void enterIdentifier(CajetaParser::IdentifierContext * /*ctx*/) override { }
        virtual void exitIdentifier(CajetaParser::IdentifierContext * /*ctx*/) override { }

        virtual void enterLocalTypeDeclaration(CajetaParser::LocalTypeDeclarationContext * /*ctx*/) override { }
        virtual void exitLocalTypeDeclaration(CajetaParser::LocalTypeDeclarationContext * /*ctx*/) override { }

        virtual void enterStatement(CajetaParser::StatementContext * /*ctx*/) override { }
        virtual void exitStatement(CajetaParser::StatementContext * /*ctx*/) override { }

        virtual void enterCatchClause(CajetaParser::CatchClauseContext * /*ctx*/) override { }
        virtual void exitCatchClause(CajetaParser::CatchClauseContext * /*ctx*/) override { }

        virtual void enterCatchType(CajetaParser::CatchTypeContext * /*ctx*/) override { }
        virtual void exitCatchType(CajetaParser::CatchTypeContext * /*ctx*/) override { }

        virtual void enterFinallyBlock(CajetaParser::FinallyBlockContext * /*ctx*/) override { }
        virtual void exitFinallyBlock(CajetaParser::FinallyBlockContext * /*ctx*/) override { }

        virtual void enterResourceSpecification(CajetaParser::ResourceSpecificationContext * /*ctx*/) override { }
        virtual void exitResourceSpecification(CajetaParser::ResourceSpecificationContext * /*ctx*/) override { }

        virtual void enterResources(CajetaParser::ResourcesContext * /*ctx*/) override { }
        virtual void exitResources(CajetaParser::ResourcesContext * /*ctx*/) override { }

        virtual void enterResource(CajetaParser::ResourceContext * /*ctx*/) override { }
        virtual void exitResource(CajetaParser::ResourceContext * /*ctx*/) override { }

        virtual void enterSwitchBlockStatementGroup(CajetaParser::SwitchBlockStatementGroupContext * /*ctx*/) override { }
        virtual void exitSwitchBlockStatementGroup(CajetaParser::SwitchBlockStatementGroupContext * /*ctx*/) override { }

        virtual void enterSwitchLabel(CajetaParser::SwitchLabelContext * /*ctx*/) override { }
        virtual void exitSwitchLabel(CajetaParser::SwitchLabelContext * /*ctx*/) override { }

        virtual void enterForControl(CajetaParser::ForControlContext * /*ctx*/) override { }
        virtual void exitForControl(CajetaParser::ForControlContext * /*ctx*/) override { }

        virtual void enterForInit(CajetaParser::ForInitContext * /*ctx*/) override { }
        virtual void exitForInit(CajetaParser::ForInitContext * /*ctx*/) override { }

        virtual void enterEnhancedForControl(CajetaParser::EnhancedForControlContext * /*ctx*/) override { }
        virtual void exitEnhancedForControl(CajetaParser::EnhancedForControlContext * /*ctx*/) override { }

        virtual void enterParExpression(CajetaParser::ParExpressionContext * /*ctx*/) override { }
        virtual void exitParExpression(CajetaParser::ParExpressionContext * /*ctx*/) override { }

        virtual void enterExpressionList(CajetaParser::ExpressionListContext * /*ctx*/) override { }
        virtual void exitExpressionList(CajetaParser::ExpressionListContext * /*ctx*/) override { }

        virtual void enterMethodCall(CajetaParser::MethodCallContext * /*ctx*/) override { }
        virtual void exitMethodCall(CajetaParser::MethodCallContext * /*ctx*/) override { }

        virtual void enterExpression(CajetaParser::ExpressionContext * /*ctx*/) override { }
        virtual void exitExpression(CajetaParser::ExpressionContext * /*ctx*/) override { }

        virtual void enterPattern(CajetaParser::PatternContext * /*ctx*/) override { }
        virtual void exitPattern(CajetaParser::PatternContext * /*ctx*/) override { }

        virtual void enterLambdaExpression(CajetaParser::LambdaExpressionContext * /*ctx*/) override { }
        virtual void exitLambdaExpression(CajetaParser::LambdaExpressionContext * /*ctx*/) override { }

        virtual void enterLambdaParameters(CajetaParser::LambdaParametersContext * /*ctx*/) override { }
        virtual void exitLambdaParameters(CajetaParser::LambdaParametersContext * /*ctx*/) override { }

        virtual void enterLambdaBody(CajetaParser::LambdaBodyContext * /*ctx*/) override { }
        virtual void exitLambdaBody(CajetaParser::LambdaBodyContext * /*ctx*/) override { }

        virtual void enterPrimary(CajetaParser::PrimaryContext * /*ctx*/) override { }
        virtual void exitPrimary(CajetaParser::PrimaryContext * /*ctx*/) override { }

        virtual void enterSwitchExpression(CajetaParser::SwitchExpressionContext * /*ctx*/) override { }
        virtual void exitSwitchExpression(CajetaParser::SwitchExpressionContext * /*ctx*/) override { }

        virtual void enterSwitchLabeledRule(CajetaParser::SwitchLabeledRuleContext * /*ctx*/) override { }
        virtual void exitSwitchLabeledRule(CajetaParser::SwitchLabeledRuleContext * /*ctx*/) override { }

        virtual void enterGuardedPattern(CajetaParser::GuardedPatternContext * /*ctx*/) override { }
        virtual void exitGuardedPattern(CajetaParser::GuardedPatternContext * /*ctx*/) override { }

        virtual void enterSwitchRuleOutcome(CajetaParser::SwitchRuleOutcomeContext * /*ctx*/) override { }
        virtual void exitSwitchRuleOutcome(CajetaParser::SwitchRuleOutcomeContext * /*ctx*/) override { }

        virtual void enterClassType(CajetaParser::ClassTypeContext * /*ctx*/) override { }
        virtual void exitClassType(CajetaParser::ClassTypeContext * /*ctx*/) override { }

        virtual void enterCreator(CajetaParser::CreatorContext * /*ctx*/) override { }
        virtual void exitCreator(CajetaParser::CreatorContext * /*ctx*/) override { }

        virtual void enterCreatedName(CajetaParser::CreatedNameContext * /*ctx*/) override { }
        virtual void exitCreatedName(CajetaParser::CreatedNameContext * /*ctx*/) override { }

        virtual void enterInnerCreator(CajetaParser::InnerCreatorContext * /*ctx*/) override { }
        virtual void exitInnerCreator(CajetaParser::InnerCreatorContext * /*ctx*/) override { }

        virtual void enterArrayCreatorRest(CajetaParser::ArrayCreatorRestContext * /*ctx*/) override { }
        virtual void exitArrayCreatorRest(CajetaParser::ArrayCreatorRestContext * /*ctx*/) override { }

        virtual void enterClassCreatorRest(CajetaParser::ClassCreatorRestContext * /*ctx*/) override { }
        virtual void exitClassCreatorRest(CajetaParser::ClassCreatorRestContext * /*ctx*/) override { }

        virtual void enterExplicitGenericInvocation(CajetaParser::ExplicitGenericInvocationContext * /*ctx*/) override { }
        virtual void exitExplicitGenericInvocation(CajetaParser::ExplicitGenericInvocationContext * /*ctx*/) override { }

        virtual void enterTypeArgumentsOrDiamond(CajetaParser::TypeArgumentsOrDiamondContext * /*ctx*/) override { }
        virtual void exitTypeArgumentsOrDiamond(CajetaParser::TypeArgumentsOrDiamondContext * /*ctx*/) override { }

        virtual void enterNonWildcardTypeArgumentsOrDiamond(CajetaParser::NonWildcardTypeArgumentsOrDiamondContext * /*ctx*/) override { }
        virtual void exitNonWildcardTypeArgumentsOrDiamond(CajetaParser::NonWildcardTypeArgumentsOrDiamondContext * /*ctx*/) override { }

        virtual void enterNonWildcardTypeArguments(CajetaParser::NonWildcardTypeArgumentsContext * /*ctx*/) override { }
        virtual void exitNonWildcardTypeArguments(CajetaParser::NonWildcardTypeArgumentsContext * /*ctx*/) override { }

        virtual void enterTypeList(CajetaParser::TypeListContext * /*ctx*/) override { }
        virtual void exitTypeList(CajetaParser::TypeListContext * /*ctx*/) override { }

        virtual void enterTypeType(CajetaParser::TypeTypeContext * /*ctx*/) override { }
        virtual void exitTypeType(CajetaParser::TypeTypeContext * /*ctx*/) override { }

        virtual void enterReferenceType(CajetaParser::ReferenceTypeContext * /*ctx*/) override { }
        virtual void exitReferenceType(CajetaParser::ReferenceTypeContext * /*ctx*/) override { }

        virtual void enterPrimitiveType(CajetaParser::PrimitiveTypeContext * /*ctx*/) override { }
        virtual void exitPrimitiveType(CajetaParser::PrimitiveTypeContext * /*ctx*/) override { }

        virtual void enterTypeArguments(CajetaParser::TypeArgumentsContext * /*ctx*/) override { }
        virtual void exitTypeArguments(CajetaParser::TypeArgumentsContext * /*ctx*/) override { }

        virtual void enterSuperSuffix(CajetaParser::SuperSuffixContext * /*ctx*/) override { }
        virtual void exitSuperSuffix(CajetaParser::SuperSuffixContext * /*ctx*/) override { }

        virtual void enterExplicitGenericInvocationSuffix(CajetaParser::ExplicitGenericInvocationSuffixContext * /*ctx*/) override { }
        virtual void exitExplicitGenericInvocationSuffix(CajetaParser::ExplicitGenericInvocationSuffixContext * /*ctx*/) override { }

        virtual void enterArguments(CajetaParser::ArgumentsContext * /*ctx*/) override { }
        virtual void exitArguments(CajetaParser::ArgumentsContext * /*ctx*/) override { }


        virtual void enterEveryRule(antlr4::ParserRuleContext * /*ctx*/) override { }
        virtual void exitEveryRule(antlr4::ParserRuleContext * /*ctx*/) override { }
        virtual void visitTerminal(antlr4::tree::TerminalNode * /*node*/) override { }
        virtual void visitErrorNode(antlr4::tree::ErrorNode * /*node*/) override { }
    };
}

#endif //CAJETA_CAJETAPARSERIRLISTENER_H
