////
//// Created by James Klappenbach on 1/20/22.
////
//
//#pragma once
//
//#include <llvm/IR/LegacyPassManager.h>
//#include <llvm/IR/Module.h>
//#include <llvm/IR/IRBuilder.h>
//#include <llvm/Support/Error.h>
//#include <llvm/Support/FileSystem.h>
//#include <llvm/Support/Host.h>
//#include <llvm/MC/TargetRegistry.h>
//#include <llvm/Target/TargetMachine.h>
//#include <llvm/Target/TargetOptions.h>
//#include <CajetaParserBaseVisitor.h>
//#include <CajetaParser.h>
//#include <llvm/Bitcode/BitcodeWriter.h>
//
//using namespace std;
//
//namespace cajeta {
//
//
//    class CajetaParserIRVisitor : public CajetaParserBaseVisitor {
//    private:
//        unique_ptr<llvm::LLVMContext> llvmContext;
//        unique_ptr<llvm::Module> module;
//        unique_ptr<llvm::IRBuilder<>> builder;
//        map<std::string, llvm::Value*> variables;
//
//        // Parsing state
//        string srcPath;
//        string targetPath;
//        ParseState parseState;
//        string package;
//        map<string, string> memberVariables;
//        map<string, list<pair<string, string>>> memberMethods;
//        deque<TypeDefinition*> classStack;
//        list<TypeDefinition*> classes;
//        set<AccessModifier>* accessModifiers;
//        std::map<std::string, llvm::StructType*> allocatedClasses;
//        Scope moduleScope;
//        Scope* currentScope;
//
//    public:
//        CajetaParserIRVisitor(string srcPath,
//                              llvm::LLVMContext* llvmContext,
//                              string targetPath) :
//                llvmContext(llvmContext),
//                module(make_unique<llvm::Module>(srcPath, *this->llvmContext)),
//                builder(make_unique<llvm::IRBuilder<>>(*this->llvmContext)),
//                moduleScope(MODULE_SCOPE, builder.get(), module.get(), llvmContext) {
//            parseState = DEFINE_CLASS;
//            auto targetTriple = llvm::sys::getDefaultTargetTriple();
//            string error;
//            auto target = llvm::TargetRegistry::lookupTarget(targetTriple, error);
//
//            if (!target) {
//                return;
//            }
//
//            auto cpu = "generic";
//            auto features = "";
//
//            llvm::TargetOptions opt;
//            auto RM = llvm::Optional<llvm::Reloc::Model>();
//            auto targetMachine = target->createTargetMachine(targetTriple, cpu, features, opt, RM);
//
//            module->setDataLayout(targetMachine->createDataLayout());
//            module->setTargetTriple(targetTriple);
//            accessModifiers = new set<AccessModifier>;
//
//            std::error_code ec;
//            string fileName;
//            llvm::raw_fd_ostream dest(fileName, ec, llvm::sys::fs::OF_None);
//
//            if (ec) {
//                error = "Could not open file: " + ec.message();
//                return;
//            }
//
//            //llvmContext->pImpl->
//
////            legacy::PassManager pass;
////            auto FileType = llvm::CGFT_ObjectFile;
////
////            if (targetMachine->addPassesToEmitFile(pass, dest, nullptr, FileType)) {
////                error = "TargetMachine can't emit a file of this type";
////                return;
////            }
////
////            pass.run(*module);
////            dest.flush();
//        }
//
//        virtual antlrcpp::Any visitCompilationUnit(CajetaParser::CompilationUnitContext* ctx) override {
//            cout << "visitCompilationUnit\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitPackageDeclaration(CajetaParser::PackageDeclarationContext* ctx) override {
//            cout << "visitPackageDeclaration\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitImportDeclaration(CajetaParser::ImportDeclarationContext* ctx) override {
//            cout << "visitPackageDeclaration\n";
//            //module->
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitTypeDeclaration(CajetaParser::TypeDeclarationContext* ctx) override {
//            cout << "visitTypeDeclaration\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitModifier(CajetaParser::ModifierContext* ctx) override {
//            accessModifiers->insert(toAccessModifier(ctx->getText()));
//            cout << "visitModifier\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitClassOrInterfaceModifier(CajetaParser::ClassOrInterfaceModifierContext* ctx) override {
//            cout << "visitClassOrInterfaceModifier\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitVariableModifier(CajetaParser::VariableModifierContext* ctx) override {
//            cout << "visitVariableModifier\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitClassDeclaration(CajetaParser::ClassDeclarationContext* ctx) override {
//            cout << "visitClassDeclaration\n";
//            TypeDefinition* structDefinition = new ClassDefinition;
//            classes.push_back(structDefinition);
//            classStack.push_front(structDefinition);
//
//            Scope* scope = new Scope(CLASS_SCOPE, builder.get(), module.get(), llvmContext.get());
////            ctx->classBody()->
////
////            structDefinition->name = ctx->identifier()->getText();
//            structDefinition->structType = llvm::StructType::create(*llvmContext, llvm::StringRef(structDefinition->name));
//            structDefinition->accessModifiers = accessModifiers;
//            accessModifiers = new set<AccessModifier>;
//            // TODO Inheritance
//            //structDefinition->typeParameters;
//
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitTypeParameters(CajetaParser::TypeParametersContext* ctx) override {
//            cout << "visitTypeParameters\n";
//            TypeDefinition* structDefinition = classStack.front();
//            std::vector<CajetaParser::TypeParameterContext *> parameters = ctx->typeParameter();
//            for (auto itr = parameters.begin(); itr != parameters.end(); itr++) {
//                CajetaParser::TypeParameterContext* ctxTypeParameter = *itr;
//                CajetaParser::IdentifierContext* ctxIdentifier = ctxTypeParameter->identifier();
//                string parameterName = ctxIdentifier->getText();
//                CajetaParser::TypeBoundContext* ctxTypeBound = ctxTypeParameter->typeBound();
//                TypeParameter* typeParameter;
//                if (ctxTypeBound) {
//                    typeParameter = new ExtendedTypeParameter(parameterName, ctxTypeBound->getText());
//                } else {
//                    typeParameter = new TypeParameter(parameterName);
//                }
//                structDefinition->typeParameters.push_back(typeParameter);
//            }
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitTypeParameter(CajetaParser::TypeParameterContext* ctx) override {
//            cout << "visitTypeParameter\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitTypeBound(CajetaParser::TypeBoundContext* ctx) override {
//            cout << "visitTypeBound\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitEnumDeclaration(CajetaParser::EnumDeclarationContext* ctx) override {
//            cout << "visitEnumDeclaration\n";
//            EnumDefinition* enumDefinition = new EnumDefinition;
//            CajetaParser::TypeListContext* ctxTypeList = ctx->typeList();
//
//            // Pull the inheritance tree
//            enumDefinition->name = ctx->identifier()->getText();
//            // TODO: figure out how we're going to have enum constants as instances of the enum type
//            // structDefinition->constants
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitEnumConstants(CajetaParser::EnumConstantsContext* ctx) override {
//            cout << "visitEnumConstants\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitEnumConstant(CajetaParser::EnumConstantContext* ctx) override {
//            cout << "visitEnumConstants\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitEnumBodyDeclarations(CajetaParser::EnumBodyDeclarationsContext* ctx) override {
//            cout << "visitEnumBodyDeclarations\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitInterfaceDeclaration(CajetaParser::InterfaceDeclarationContext* ctx) override {
//            cout << "visitInterfaceDeclaration\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitClassBody(CajetaParser::ClassBodyContext* ctx) override {
//            cout << "visitClassBody\n";
//            // TODO Add Class body handling here
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitInterfaceBody(CajetaParser::InterfaceBodyContext* ctx) override {
//            cout << "visitInterfaceBody\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitClassBodyDeclaration(CajetaParser::ClassBodyDeclarationContext* ctx) override {
//            cout << "visitClassBodyDeclaration\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitMemberDeclaration(CajetaParser::MemberDeclarationContext* ctx) override {
//            cout << "visitMemberDeclaration\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitMethodDeclaration(CajetaParser::MethodDeclarationContext* ctx) override {
//            cout << "visitMethodDeclaration\n";
//            CajetaParser::TypeTypeContext* ctxReturnType = ctx->typeTypeOrVoid()->typeType();
//            CajetaParser::PrimitiveTypeContext* ctxPrimitiveType = ctxReturnType->primitiveType();
//            if (ctxPrimitiveType != NULL) {
//
//            } else {
//                CajetaParser::ClassOrInterfaceTypeContext* ctxClassOrInterface = ctxReturnType->classOrInterfaceType();
//                if (ctxClassOrInterface != NULL) {
//
//                } else {
//                    ctxReturnType->referenceType();
//                }
//            }
//            CajetaParser::ReferenceTypeContext* ctxReferenceType = ctxReturnType->referenceType();
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitMethodBody(CajetaParser::MethodBodyContext* ctx) override {
//            cout << "visitMethodBody\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitTypeTypeOrVoid(CajetaParser::TypeTypeOrVoidContext* ctx) override {
//            cout << "visitTypeTypeOrVoid\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitGenericMethodDeclaration(CajetaParser::GenericMethodDeclarationContext* ctx) override {
//            cout << "visitGenericMethodDeclaration\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitGenericConstructorDeclaration(CajetaParser::GenericConstructorDeclarationContext* ctx) override {
//            cout << "visitGenericConstructorDeclaration\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitConstructorDeclaration(CajetaParser::ConstructorDeclarationContext* ctx) override {
//            cout << "visitConstructorDeclaration\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitFieldDeclaration(CajetaParser::FieldDeclarationContext* ctx) override {
//            cout << "visitFieldDeclaration\n";
////            CajetaParser::TypeTypeContext ctxType = ctx->typeType();
////            ctxType->
//            // TODO Add initial class modifier handling here
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitInterfaceBodyDeclaration(CajetaParser::InterfaceBodyDeclarationContext* ctx) override {
//            cout << "visitInterfaceBodyDeclaration\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitInterfaceMemberDeclaration(CajetaParser::InterfaceMemberDeclarationContext* ctx) override {
//            cout << "visitInterfaceMemberDeclaration\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitConstDeclaration(CajetaParser::ConstDeclarationContext* ctx) override {
//            cout << "visitConstDeclaration\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitConstantDeclarator(CajetaParser::ConstantDeclaratorContext* ctx) override {
//            cout << "visitConstantDeclarator\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitInterfaceMethodDeclaration(CajetaParser::InterfaceMethodDeclarationContext* ctx) override {
//            cout << "visitInterfaceMethodDeclaration\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitInterfaceMethodModifier(CajetaParser::InterfaceMethodModifierContext* ctx) override {
//            cout << "visitInterfaceMethodModifier\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitGenericInterfaceMethodDeclaration(CajetaParser::GenericInterfaceMethodDeclarationContext* ctx) override {
//            cout << "visitGenericInterfaceMethodDeclaration\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitInterfaceCommonBodyDeclaration(CajetaParser::InterfaceCommonBodyDeclarationContext* ctx) override {
//            cout << "visitInterfaceCommonBodyDeclaration\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitVariableDeclarators(CajetaParser::VariableDeclaratorsContext* ctx) override {
//            cout << "visitVariableDeclarators\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitVariableDeclarator(CajetaParser::VariableDeclaratorContext* ctx) override {
//            cout << "visitVariableDeclarator\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitVariableDeclaratorId(CajetaParser::VariableDeclaratorIdContext* ctx) override {
//            cout << "visitVariableDeclaratorId\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitVariableInitializer(CajetaParser::VariableInitializerContext* ctx) override {
//            cout << "visitVariableInitializer\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitArrayInitializer(CajetaParser::ArrayInitializerContext* ctx) override {
//            cout << "visitArrayInitializer\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitClassOrInterfaceType(CajetaParser::ClassOrInterfaceTypeContext* ctx) override {
//            cout << "visitClassOrInterfaceType\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitTypeArgument(CajetaParser::TypeArgumentContext* ctx) override {
//            cout << "visitTypeArgument\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitQualifiedNameList(CajetaParser::QualifiedNameListContext* ctx) override {
//            cout << "visitQualifiedNameList\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitFormalParameters(CajetaParser::FormalParametersContext* ctx) override {
//            cout << "visitFormalParameters\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitReceiverParameter(CajetaParser::ReceiverParameterContext* ctx) override {
//            cout << "visitReceiverParameter\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitFormalParameterList(CajetaParser::FormalParameterListContext* ctx) override {
//            cout << "visitFormalParameterList\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitFormalParameter(CajetaParser::FormalParameterContext* ctx) override {
//            cout << "visitFormalParameter\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitLastFormalParameter(CajetaParser::LastFormalParameterContext* ctx) override {
//            cout << "visitLastFormalParameter\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitLambdaLVTIList(CajetaParser::LambdaLVTIListContext* ctx) override {
//            cout << "visitLambdaLVTIList\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitLambdaLVTIParameter(CajetaParser::LambdaLVTIParameterContext* ctx) override {
//            cout << "visitLambdaLVTIParameter\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitQualifiedName(CajetaParser::QualifiedNameContext* ctx) override {
//            cout << "visitQualifiedName\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitLiteral(CajetaParser::LiteralContext* ctx) override {
//            cout << "visitLiteral\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitIntegerLiteral(CajetaParser::IntegerLiteralContext* ctx) override {
//            cout << "visitIntegerLiteral\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitFloatLiteral(CajetaParser::FloatLiteralContext* ctx) override {
//            cout << "visitFloatLiteral\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitAltAnnotationQualifiedName(CajetaParser::AltAnnotationQualifiedNameContext* ctx) override {
//            cout << "visitAltAnnotationQualifiedName\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitAnnotation(CajetaParser::AnnotationContext* ctx) override {
//            cout << "visitAnnotation\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitElementValuePairs(CajetaParser::ElementValuePairsContext* ctx) override {
//            cout << "visitElementValuePairs\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitElementValuePair(CajetaParser::ElementValuePairContext* ctx) override {
//            cout << "visitElementValuePair\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitElementValue(CajetaParser::ElementValueContext* ctx) override {
//            cout << "visitElementValue\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitElementValueArrayInitializer(CajetaParser::ElementValueArrayInitializerContext* ctx) override {
//            cout << "visitElementValueArrayInitializer\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitAnnotationTypeDeclaration(CajetaParser::AnnotationTypeDeclarationContext* ctx) override {
//            cout << "visitAnnotationTypeDeclaration\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitAnnotationTypeBody(CajetaParser::AnnotationTypeBodyContext* ctx) override {
//            cout << "visitAnnotationTypeBody\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitAnnotationTypeElementDeclaration(CajetaParser::AnnotationTypeElementDeclarationContext* ctx) override {
//            cout << "visitAnnotationTypeElementDeclaration\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitAnnotationTypeElementRest(CajetaParser::AnnotationTypeElementRestContext* ctx) override {
//            cout << "visitAnnotationTypeElementRest\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitAnnotationMethodOrConstantRest(CajetaParser::AnnotationMethodOrConstantRestContext* ctx) override {
//            cout << "visitAnnotationMethodOrConstantRest\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitAnnotationMethodRest(CajetaParser::AnnotationMethodRestContext* ctx) override {
//            cout << "visitAnnotationMethodRest\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitAnnotationConstantRest(CajetaParser::AnnotationConstantRestContext* ctx) override {
//            cout << "visitAnnotationConstantRest\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitDefaultValue(CajetaParser::DefaultValueContext* ctx) override {
//            cout << "visitDefaultValue\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitModuleDeclaration(CajetaParser::ModuleDeclarationContext* ctx) override {
//            cout << "visitModuleDeclaration\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitModuleBody(CajetaParser::ModuleBodyContext* ctx) override {
//            cout << "visitModuleBody\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitModuleDirective(CajetaParser::ModuleDirectiveContext* ctx) override {
//            cout << "visitModuleDirective\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitRequiresModifier(CajetaParser::RequiresModifierContext* ctx) override {
//            cout << "visitRequiresModifier\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitRecordDeclaration(CajetaParser::RecordDeclarationContext* ctx) override {
//            cout << "visitRecordDeclaration\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitRecordHeader(CajetaParser::RecordHeaderContext* ctx) override {
//            cout << "visitRecordHeader\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitRecordComponentList(CajetaParser::RecordComponentListContext* ctx) override {
//            cout << "visitRecordComponentList\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitRecordComponent(CajetaParser::RecordComponentContext* ctx) override {
//            cout << "visitRecordComponent\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitRecordBody(CajetaParser::RecordBodyContext* ctx) override {
//            cout << "visitRecordBody\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitBlock(CajetaParser::BlockContext* ctx) override {
//            cout << "visitBlock\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitBlockStatement(CajetaParser::BlockStatementContext* ctx) override {
//            cout << "visitBlockStatement\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitLocalVariableDeclaration(CajetaParser::LocalVariableDeclarationContext* ctx) override {
//            cout << "visitLocalVariableDeclaration\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitIdentifier(CajetaParser::IdentifierContext* ctx) override {
//            cout << "visitIdentifier\n";
////            switch (parseState) {
////                case DEFINE_CLASS:
////                    name = ctx->getText();
////                    break;
////                case DEFINE_VARIABLE:
////                    break;
////                case DEFINE_METHOD_SIG:
////                    break;
////                case DEFINE_METHOD:
////                    break;
////            }
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitLocalTypeDeclaration(CajetaParser::LocalTypeDeclarationContext* ctx) override {
//            cout << "visitLocalTypeDeclaration\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitStatement(CajetaParser::StatementContext* ctx) override {
//            cout << "visitStatement\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitCatchClause(CajetaParser::CatchClauseContext* ctx) override {
//            cout << "visitCatchClause\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitCatchType(CajetaParser::CatchTypeContext* ctx) override {
//            cout << "visitCatchType\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitFinallyBlock(CajetaParser::FinallyBlockContext* ctx) override {
//            cout << "visitFinallyBlock\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitResourceSpecification(CajetaParser::ResourceSpecificationContext* ctx) override {
//            cout << "visitResourceSpecification\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitResources(CajetaParser::ResourcesContext* ctx) override {
//            cout << "visitResources\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitResource(CajetaParser::ResourceContext* ctx) override {
//            cout << "visitResource\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitSwitchBlockStatementGroup(CajetaParser::SwitchBlockStatementGroupContext* ctx) override {
//            cout << "visitSwitchBlockStatementGroup\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitSwitchLabel(CajetaParser::SwitchLabelContext* ctx) override {
//            cout << "visitSwitchLabel\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitForControl(CajetaParser::ForControlContext* ctx) override {
//            cout << "visitForControl\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitForInit(CajetaParser::ForInitContext* ctx) override {
//            cout << "visitForInit\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitEnhancedForControl(CajetaParser::EnhancedForControlContext* ctx) override {
//            cout << "visitEnhancedForControl\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitParExpression(CajetaParser::ParExpressionContext* ctx) override {
//            cout << "visitParExpression\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitExpressionList(CajetaParser::ExpressionListContext* ctx) override {
//            cout << "visitExpressionList\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitMethodCall(CajetaParser::MethodCallContext* ctx) override {
//            cout << "visitMethodCall\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitExpression(CajetaParser::ExpressionContext* ctx) override {
//            cout << "visitExpression\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitPattern(CajetaParser::PatternContext* ctx) override {
//            cout << "visitPattern\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitLambdaExpression(CajetaParser::LambdaExpressionContext* ctx) override {
//            cout << "visitLambdaExpression\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitLambdaParameters(CajetaParser::LambdaParametersContext* ctx) override {
//            cout << "visitLambdaParameters\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitLambdaBody(CajetaParser::LambdaBodyContext* ctx) override {
//            cout << "visitLambdaBody\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitPrimary(CajetaParser::PrimaryContext* ctx) override {
//            cout << "visitPrimary\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitSwitchExpression(CajetaParser::SwitchExpressionContext* ctx) override {
//            cout << "visitSwitchExpression\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitSwitchLabeledRule(CajetaParser::SwitchLabeledRuleContext* ctx) override {
//            cout << "visitSwitchLabeledRule\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitGuardedPattern(CajetaParser::GuardedPatternContext* ctx) override {
//            cout << "visitGuardedPattern\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitSwitchRuleOutcome(CajetaParser::SwitchRuleOutcomeContext* ctx) override {
//            cout << "visitSwitchRuleOutcome\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitClassType(CajetaParser::ClassTypeContext* ctx) override {
//            cout << "visitClassType\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitCreator(CajetaParser::CreatorContext* ctx) override {
//            cout << "visitCreator\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitCreatedName(CajetaParser::CreatedNameContext* ctx) override {
//            cout << "visitCreatedName\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitInnerCreator(CajetaParser::InnerCreatorContext* ctx) override {
//            cout << "visitInnerCreator\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitArrayCreatorRest(CajetaParser::ArrayCreatorRestContext* ctx) override {
//            cout << "visitArrayCreatorRest\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitClassCreatorRest(CajetaParser::ClassCreatorRestContext* ctx) override {
//            cout << "visitClassCreatorRest\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitExplicitGenericInvocation(CajetaParser::ExplicitGenericInvocationContext* ctx) override {
//            cout << "visitExplicitGenericInvocation\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitTypeArgumentsOrDiamond(CajetaParser::TypeArgumentsOrDiamondContext* ctx) override {
//            cout << "visitTypeArgumentsOrDiamond\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitNonWildcardTypeArgumentsOrDiamond(CajetaParser::NonWildcardTypeArgumentsOrDiamondContext* ctx) override {
//            cout << "visitNonWildcardTypeArgumentsOrDiamond\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitNonWildcardTypeArguments(CajetaParser::NonWildcardTypeArgumentsContext* ctx) override {
//            cout << "visitNonWildcardTypeArguments\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitTypeList(CajetaParser::TypeListContext* ctx) override {
//            cout << "visitTypeList\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitTypeType(CajetaParser::TypeTypeContext* ctx) override {
//            cout << "visitTypeType\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitPrimitiveType(CajetaParser::PrimitiveTypeContext* ctx) override {
//            cout << "visitPrimitiveType\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitTypeArguments(CajetaParser::TypeArgumentsContext* ctx) override {
//            cout << "visitTypeArguments\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitSuperSuffix(CajetaParser::SuperSuffixContext* ctx) override {
//            cout << "visitSuperSuffix\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitExplicitGenericInvocationSuffix(CajetaParser::ExplicitGenericInvocationSuffixContext* ctx) override {
//            cout << "visitExplicitGenericInvocationSuffix\n";
//            return visitChildren(ctx);
//        }
//
//        virtual antlrcpp::Any visitArguments(CajetaParser::ArgumentsContext* ctx) override {
//            cout << "visitArguments\n";
//            return visitChildren(ctx);
//        }
//
//        void onComplete() {
//            module->setSourceFileName(srcPath);
//
//            // TODO: Add this back in: llvm::StructType::create(*llvmContext, StringRef(""));
////            WriteBitcodeToFile(module, const Module &M, raw_ostream &Out,
////                               bool ShouldPreserveUseListOrder = false,
////                               const ModuleSummaryIndex *Index = nullptr,
////                               bool GenerateHash = false,
////                               ModuleHash *ModHash = nullptr)
//        }
//    };
//}  // namespace cajeta
//
