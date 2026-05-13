
// Generated of /Users/julian/code/cpp/code/antlr4/CajetaParser.g4 by ANTLR 4.9.3

#pragma once


#include "antlr4-runtime.h"
#include "CajetaParserVisitor.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaStruct.h"
#include <any>
#include "cajeta/asn/Block.h"
#include "cajeta/asn/Statement.h"
#include "cajeta/asn/expression/Expression.h"
#include "cajeta/asn/LocalVariableDeclaration.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/asn/ClassBodyDeclaration.h"


namespace cajeta {

    /**
     * This class provides an empty implementation of CajetaLlvmVisitor, which can be
     * extended to getOrCreate a visitor which only needs to handle a subset of the available methods.
     */
    class CajetaLlvmVisitor : public CajetaParserVisitor {
    private:
        CajetaModulePtr pModule;
    public:
        CajetaLlvmVisitor(CajetaModulePtr module) {
            this->pModule = module;
        }

        CajetaModulePtr getCajetaModule() const {
            return pModule;
        }

        virtual std::any visitCompilationUnit(CajetaParser::CompilationUnitContext* ctx) override {
            pModule->onPackageDeclaration(ctx->packageDeclaration());
            for (auto& importDeclarationContext: ctx->importDeclaration()) {
                pModule->onImportDeclaration(importDeclarationContext);
            }
            for (auto& typeDeclarationContext: ctx->typeDeclaration()) {
                pModule->onStructureDeclaration(visitChildren(typeDeclarationContext));
            }
            return std::any(nullptr);
        }

        virtual std::any visitPackageDeclaration(CajetaParser::PackageDeclarationContext* ctx) override {
            pModule->onPackageDeclaration(ctx);
            return visitChildren(ctx);
        }

        virtual std::any visitImportDeclaration(CajetaParser::ImportDeclarationContext* ctx) override {
            pModule->onImportDeclaration(ctx);
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
            for (auto& structure: pModule->getStructureStack()) {
                packageAdj.append(".");
                packageAdj.append(structure->getQName()->getTypeName());
            }
            QualifiedNamePtr qName = QualifiedName::getOrInsert(name, pModule->getQName()->getPackageName() + packageAdj);
            list<QualifiedNamePtr> qExtended;
            list<QualifiedNamePtr> qImplemented;
            // Grammar: `(EXTENDS typeList)? (IMPLEMENTS typeList)? (PERMITS typeList)?`
            // ANTLR exposes typeLists in source order; match each to its
            // keyword by start-token index. Sealed-class PERMITS is parsed
            // but its typeList is ignored in v1 (GAP-11).
            auto kwIdx = [](antlr4::tree::TerminalNode* n) -> ssize_t {
                return n && n->getSymbol() ? (ssize_t) n->getSymbol()->getTokenIndex() : -1;
            };
            ssize_t extKw = kwIdx(ctx->EXTENDS());
            ssize_t implKw = kwIdx(ctx->IMPLEMENTS());
            ssize_t permKw = kwIdx(ctx->PERMITS());
            for (auto* tl : ctx->typeList()) {
                ssize_t tlIdx = tl->getStart()
                    ? (ssize_t) tl->getStart()->getTokenIndex() : -1;
                // Determine which keyword this typeList follows by picking
                // the largest keyword-index that's still less than tlIdx.
                ssize_t best = -1;
                int which = -1; // 0=extends, 1=implements, 2=permits
                if (extKw >= 0 && extKw < tlIdx && extKw > best) { best = extKw; which = 0; }
                if (implKw >= 0 && implKw < tlIdx && implKw > best) { best = implKw; which = 1; }
                if (permKw >= 0 && permKw < tlIdx && permKw > best) { best = permKw; which = 2; }
                list<QualifiedNamePtr>* bucket = nullptr;
                if (which == 0) bucket = &qExtended;
                else if (which == 1) bucket = &qImplemented;
                if (!bucket) continue;
                for (auto& tt : tl->typeType()) {
                    bucket->push_back(QualifiedName::fromContext(tt->classOrInterfaceType()));
                }
            }
            CajetaClassPtr structure = make_shared<CajetaClass>(pModule, qName, qExtended, qImplemented);

            // Template parameters — capture name + optional `extends` bounds.
            // Bounds are resolved to QualifiedNamePtrs here so we don't need
            // to hold the parse tree past per-module build. The class becomes
            // a template (non-instantiable until referenced with concrete args
            // via `instantiate(...)`). We also capture the raw source text of
            // the enclosing typeDeclaration so the parse tree can be released
            // when this visit pass ends — re-parsing the snippet on demand is
            // cheap and the result is cached per instantiation. ANTLR context
            // nodes carry parent links to the compilation unit, so pinning a
            // single class would transitively pin the whole file's tree.
            if (auto* tps = ctx->typeParameters()) {
                vector<TypeParameter> params;
                for (auto* tp : tps->typeParameter()) {
                    TypeParameter param(tp->identifier()->getText());
                    if (auto* bound = tp->typeBound()) {
                        for (auto* tt : bound->typeType()) {
                            if (auto* coi = tt->classOrInterfaceType()) {
                                param.bounds.push_back(QualifiedName::fromContext(coi));
                            }
                        }
                    }
                    params.push_back(std::move(param));
                }
                structure->setTypeParameters(std::move(params));

                antlr4::ParserRuleContext* enclosing = ctx;
                if (auto* td = dynamic_cast<CajetaParser::TypeDeclarationContext*>(ctx->parent)) {
                    enclosing = td;
                }
                auto* startTok = enclosing->getStart();
                auto* stopTok = enclosing->getStop();
                if (startTok && stopTok && startTok->getInputStream()) {
                    antlr4::misc::Interval interval(
                        startTok->getStartIndex(), stopTok->getStopIndex());
                    structure->setTemplateSource(
                        startTok->getInputStream()->getText(interval));
                }
            }

            // Capture user-supplied annotations from the enclosing
            // typeDeclaration (e.g. `@Loggable public class Foo`). v1 stores
            // them by name only — element-value pairs (`@MyAnn(key=v)`) are
            // parsed but not yet captured. Annotations are accessible to
            // future reflection / RTTI paths via Annotatable::getAnnotations.
            if (auto* typeDecl = dynamic_cast<CajetaParser::TypeDeclarationContext*>(ctx->parent)) {
                for (auto* mod : typeDecl->classOrInterfaceModifier()) {
                    auto* ann = mod->annotation();
                    if (!ann) continue;
                    QualifiedNamePtr qn;
                    if (ann->qualifiedName()) {
                        qn = QualifiedName::fromContext(ann->qualifiedName());
                    } else if (auto* alt = ann->altAnnotationQualifiedName()) {
                        // `pkg.@MyAnn` form: package prefix lives in
                        // identifier(0..n-2), the annotation name is the
                        // last identifier (after the '@'). v1 takes only
                        // the leaf name.
                        const auto& ids = alt->identifier();
                        if (!ids.empty()) {
                            qn = QualifiedName::getOrInsert(
                                ids.back()->getText(), "");
                        }
                    }
                    if (qn) structure->addAnnotation(qn);
                }
            }

            pModule->getStructureStack().push_back(structure);
            // Pre-register the class in canonicalMap (under both canonical
            // and short typeName) so self-references inside the body
            // resolve — e.g. `Vector operator+ (Vector other)` inside class
            // Vector. The actual generatePrototype below will overwrite
            // these placeholder entries with the same class (idempotent).
            // Templates handle this in their own path via instantiate; skip
            // for templates here.
            if (!structure->isTemplate()) {
                CajetaType::getCanonicalMap()[qName->toCanonical()] =
                    static_pointer_cast<CajetaType>(structure);
                CajetaType::getCanonicalMap()[qName->getTypeName()] =
                    static_pointer_cast<CajetaType>(structure);
            }

            // For templates, skip the body walk entirely. The body contains
            // unresolved type-parameter references (`T value`, `T method()`)
            // that FormalParameter / CajetaType resolution can't handle in
            // the original parse pass. The captured snippet is the source of
            // truth for the body — it gets re-parsed under a substitution
            // map by `instantiate(...)`, where T is bound to a concrete type.
            // Skipping here also keeps the template out of getAllMethods'
            // codegen worklist by way of having no methods at all.
            if (!structure->isTemplate()) {
                structure->setClassBody(std::any_cast<ClassBodyDeclarationPtr>(visitChildren(ctx)));
            }
            structure->generatePrototype();
            pModule->getStructureStack().pop_back();
            CajetaModule::getStructureToModule()[structure->getQName()->toCanonical()] = pModule;
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

        virtual std::any visitStructDeclaration(CajetaParser::StructDeclarationContext* ctx) override {
            // POD struct declaration. Mirrors visitClassDeclaration's flow but
            // produces a CajetaStruct (packed layout by default, no vtable/RTTI,
            // view constructor synthesized on demand by MethodCallExpression).
            string name = ctx->identifier()->getText();
            string packageAdj;
            for (auto& structure : pModule->getStructureStack()) {
                packageAdj.append(".");
                packageAdj.append(structure->getQName()->getTypeName());
            }
            QualifiedNamePtr qName = QualifiedName::getOrInsert(
                name, pModule->getQName()->getPackageName() + packageAdj);
            auto structure = make_shared<CajetaStruct>(pModule, qName);

            // Pull wire-format annotations off the enclosing typeDeclaration.
            // The grammar parses them as classOrInterfaceModifier* before the
            // structDeclaration; from here we look upward at the parent and
            // scan its modifier list. See WireFormats.md § Endianness / §
            // Alignment for semantics.
            if (auto* typeDecl = dynamic_cast<CajetaParser::TypeDeclarationContext*>(ctx->parent)) {
                for (auto* mod : typeDecl->classOrInterfaceModifier()) {
                    auto* ann = mod->annotation();
                    if (!ann) continue;
                    string aName = ann->qualifiedName()
                        ? ann->qualifiedName()->getText()
                        : (ann->altAnnotationQualifiedName()
                            ? ann->altAnnotationQualifiedName()->getText()
                            : string());
                    if (aName == "BigEndian") {
                        structure->setEndianness(StructEndianness::Big);
                    } else if (aName == "LittleEndian") {
                        structure->setEndianness(StructEndianness::Little);
                    } else if (aName == "Align") {
                        // The argument should be `natural`. We treat any
                        // Align(...) as a request for natural alignment in v1.
                        structure->setAlignment(StructAlignment::Natural);
                    }
                }
            }

            pModule->getStructureStack().push_back(structure);
            structure->setClassBody(std::any_cast<ClassBodyDeclarationPtr>(visitChildren(ctx)));
            structure->generatePrototype();
            pModule->getStructureStack().pop_back();
            CajetaModule::getStructureToModule()[structure->getQName()->toCanonical()] = pModule;
            return static_pointer_cast<CajetaClass>(structure);
        }

        virtual std::any visitEnumDeclaration(CajetaParser::EnumDeclarationContext* ctx) override {
            // v1 enum: each constant gets an int32 ordinal (0, 1, 2, ...). The
            // enum type itself registers as an i32-backed CajetaType under its
            // qName so `MyEnum x` declares an int32 slot; `MyEnum.NAME` is
            // resolved at DotExpression codegen to an i32 constant via the
            // CajetaType::enumConstants registry.
            //
            // Not yet supported (deferred):
            //  - constants with arguments: `MONDAY(1)`
            //  - enum bodies with methods
            //  - `implements` clause on enum
            string name = ctx->identifier()->getText();
            string packageAdj;
            for (auto& s : pModule->getStructureStack()) {
                packageAdj.append(".");
                packageAdj.append(s->getQName()->getTypeName());
            }
            QualifiedNamePtr qName = QualifiedName::getOrInsert(
                name, pModule->getQName()->getPackageName() + packageAdj);

            // Register the enum as an i32-backed primitive. shareLlvmType=false
            // because i32 already owns the typeMap[i32] slot — we don't want
            // to clobber it.
            llvm::Type* i32Ty = llvm::Type::getInt32Ty(*pModule->getLlvmContext());
            auto enumType = CajetaType::create(qName, i32Ty,
                INT_FLAG | SIGNED_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG
                    | BIT_32_FLAG | ENUM_FLAG,
                /*shareLlvmType=*/false);
            // Also register under the short typeName so an unqualified `Color`
            // reference at a type-use site resolves without needing the full
            // canonical (matches how CajetaClass::generatePrototype registers
            // both forms for class names).
            CajetaType::getCanonicalMap()[qName->getTypeName()] = enumType;

            // Walk constants in declared order and assign sequential ordinals.
            int32_t ordinal = 0;
            if (auto* constants = ctx->enumConstants()) {
                for (auto* ec : constants->enumConstant()) {
                    string constName = ec->identifier()->getText();
                    CajetaType::registerEnumConstant(name, constName, ordinal++);
                }
            }
            return std::any(nullptr);
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
            // Build a CajetaClass tagged isInterface=true. The interface body
            // holds method *signatures* (abstract — no LLVM function), and
            // implementing classes' vtables get one entry per interface
            // method pointing to the class's concrete implementation.
            //
            // v1 limits:
            //  - No default methods (interface methods with bodies)
            //  - No constDeclaration
            //  - No nested types
            //  - No templated interface methods (GAP-4)
            //  - `extends I1, I2` parses but interface-extends-interface chains
            //    are not yet resolved
            string packageAdj;
            string name = ctx->identifier()->getText();
            for (auto& s : pModule->getStructureStack()) {
                packageAdj.append(".");
                packageAdj.append(s->getQName()->getTypeName());
            }
            QualifiedNamePtr qName = QualifiedName::getOrInsert(
                name, pModule->getQName()->getPackageName() + packageAdj);
            list<QualifiedNamePtr> qExtended;
            list<QualifiedNamePtr> qImplemented;
            if (auto* tl = ctx->typeList()) {
                for (auto* tt : tl->typeType()) {
                    qExtended.push_back(QualifiedName::fromContext(tt->classOrInterfaceType()));
                }
            }

            CajetaClassPtr interface = make_shared<CajetaClass>(
                pModule, qName, qExtended, qImplemented);
            interface->setIsInterface(true);

            pModule->getStructureStack().push_back(interface);

            // Build abstract Methods for each interfaceMethodDeclaration.
            // We sidestep visitClassBody/visitMethodDeclaration because those
            // expect a real method body; interface methods have either `;`
            // or a default block (the latter is deferred).
            auto classBody = make_shared<ClassBodyDeclaration>(ctx->getStart());
            auto* body = ctx->interfaceBody();
            if (body) {
                for (auto* bd : body->interfaceBodyDeclaration()) {
                    auto* md = bd->interfaceMemberDeclaration();
                    if (!md) continue;
                    auto* imd = md->interfaceMethodDeclaration();
                    if (!imd) continue;
                    auto* common = imd->interfaceCommonBodyDeclaration();
                    if (!common) continue;
                    string methodName = common->identifier()->getText();
                    vector<FormalParameterPtr> formals;
                    if (auto* fps = common->formalParameters()) {
                        if (auto* list = fps->formalParameterList()) {
                            for (auto* fp : list->formalParameter()) {
                                if (auto p = FormalParameter::fromContext(fp, pModule)) {
                                    formals.push_back(p);
                                }
                            }
                        }
                    }
                    CajetaTypePtr returnType = CajetaType::fromContext(
                        common->typeTypeOrVoid(), pModule);
                    MethodPtr method = Method::create(
                        pModule, methodName, returnType, formals,
                        /*block=*/nullptr, interface);
                    method->setAbstract(true);
                    classBody->getDeclarations().push_back(
                        make_shared<MethodDeclaration>(method, common->getStart()));
                }
            }
            interface->setClassBody(classBody);
            interface->generatePrototype();

            pModule->getStructureStack().pop_back();
            CajetaModule::getStructureToModule()[interface->getQName()->toCanonical()] = pModule;
            return interface;
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
            // `int32 operator+ (Vector other) { return ... }` and friends.
            // Build a Method whose *name* is `operator<symbol>` so the
            // BinaryOpExpression / PrefixExpression codegen can look it up
            // via the same machinery as a regular method. The receiver
            // (`this`) is implicit, as for any non-static method.
            //
            // v1 supports the common arithmetic, comparison, and compound-
            // assignment operators. Less common ops (>>>, bitwise &/|/^,
            // their compound forms) parse but their callers haven't been
            // taught yet — the methods are still registered for the day
            // somebody wires them in.
            const char* sym = "?";
            if (ctx->ADD()) sym = "+";
            else if (ctx->SUB()) sym = "-";
            else if (ctx->MUL()) sym = "*";
            else if (ctx->DIV()) sym = "/";
            else if (ctx->MOD()) sym = "%";
            else if (ctx->EQUAL()) sym = "==";
            else if (ctx->NOTEQUAL()) sym = "!=";
            else if (ctx->LT()) sym = "<";
            else if (ctx->GT()) sym = ">";
            else if (ctx->LE()) sym = "<=";
            else if (ctx->GE()) sym = ">=";
            else if (ctx->INC()) sym = "++";
            else if (ctx->DEC()) sym = "--";
            else if (ctx->ASSIGN()) sym = "=";
            else if (ctx->ADD_ASSIGN()) sym = "+=";
            else if (ctx->SUB_ASSIGN()) sym = "-=";
            else if (ctx->MUL_ASSIGN()) sym = "*=";
            else if (ctx->DIV_ASSIGN()) sym = "/=";
            else if (ctx->MOD_ASSIGN()) sym = "%=";
            else if (ctx->AND()) sym = "&&";
            else if (ctx->OR()) sym = "||";
            else if (ctx->BITAND()) sym = "&";
            else if (ctx->BITOR()) sym = "|";
            else if (ctx->CARET()) sym = "^";
            else if (ctx->AND_ASSIGN()) sym = "&=";
            else if (ctx->OR_ASSIGN()) sym = "|=";
            else if (ctx->XOR_ASSIGN()) sym = "^=";
            else if (ctx->LSHIFT_ASSIGN()) sym = "<<=";
            else if (ctx->RSHIFT_ASSIGN()) sym = ">>=";
            else if (ctx->URSHIFT_ASSIGN()) sym = ">>>=";

            string methodName = string("operator") + sym;
            vector<FormalParameterPtr> formals;
            if (auto* fps = ctx->formalParameters()) {
                if (auto* list = fps->formalParameterList()) {
                    for (auto* fp : list->formalParameter()) {
                        if (auto p = FormalParameter::fromContext(fp, pModule)) {
                            formals.push_back(p);
                        }
                    }
                }
            }
            CajetaTypePtr returnType = CajetaType::fromContext(ctx->typeType(), pModule);
            BlockPtr block;
            if (ctx->methodBody()) {
                auto bodyAny = visitMethodBody(ctx->methodBody());
                if (bodyAny.has_value()) {
                    try { block = any_cast<BlockPtr>(bodyAny); }
                    catch (const std::bad_any_cast&) { /* `;` form — no block */ }
                }
            }
            MethodPtr method = Method::create(
                pModule, methodName, returnType, formals, block,
                pModule->getStructureStack().front());
            return static_pointer_cast<MemberDeclaration>(
                make_shared<MethodDeclaration>(method, ctx->getStart()));
        }

        virtual std::any visitTemplatedOperatorOverloadDeclaration(CajetaParser::TemplatedOperatorOverloadDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitMethodDeclaration(CajetaParser::MethodDeclarationContext* ctx) override {
            string name = ctx->identifier()->getText();
            vector<FormalParameterPtr> formalParameters;
            bool varargs = false;
            if (auto* fpList = ctx->formalParameters()->formalParameterList()) {
                for (auto& fpCtx : fpList->formalParameter()) {
                    if (auto p = FormalParameter::fromContext(fpCtx, pModule)) {
                        formalParameters.push_back(p);
                    }
                }
                if (auto* lastFp = fpList->lastFormalParameter()) {
                    if (auto p = FormalParameter::fromContext(lastFp, pModule)) {
                        formalParameters.push_back(p);
                        varargs = true;
                    }
                }
            }
            CajetaTypePtr returnType = CajetaType::fromContext(ctx->typeTypeOrVoid(), pModule);
            BlockPtr block = any_cast<BlockPtr>(visitMethodBody(ctx->methodBody()));
            MethodPtr method = Method::create(
                this->pModule,
                name,
                returnType,
                formalParameters,
                block,
                pModule->getStructureStack().front());
            method->setVarargs(varargs);
            // `#T foo()` — return transfers ownership. The grammar puts the `#`
            // on typeTypeOrVoid (`REFERENCE? typeType`); see MemoryModel.md.
            if (ctx->typeTypeOrVoid() && ctx->typeTypeOrVoid()->REFERENCE() != nullptr) {
                method->setReturnsOwnership(true);
            }
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
        visitTemplatedMethodDeclaration(CajetaParser::TemplatedMethodDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any
        visitTemplatedConstructorDeclaration(CajetaParser::TemplatedConstructorDeclarationContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitConstructorDeclaration(CajetaParser::ConstructorDeclarationContext* ctx) override {
            string name = ctx->identifier()->getText();
            vector<FormalParameterPtr> formalParameters;
            if (ctx->formalParameters()->formalParameterList()) {
                for (auto& formalParameterContext: ctx->formalParameters()->formalParameterList()->formalParameter()) {
                    formalParameters.push_back(FormalParameter::fromContext(formalParameterContext, pModule));
                }
            }
            BlockPtr block = any_cast<BlockPtr>(visitBlock(ctx->constructorBody));
            MethodPtr method = Method::create(pModule, name,
                CajetaType::of("void"),
                formalParameters,
                block,
                pModule->getStructureStack().back());
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
        visitTemplatedInterfaceMethodDeclaration(CajetaParser::TemplatedInterfaceMethodDeclarationContext* ctx) override {
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
                /*isReference=*/false,
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
            // Return as InitializerPtr so callers' any_cast<InitializerPtr>
            // succeeds — make_shared<ArrayInitializer> would otherwise hand
            // back shared_ptr<ArrayInitializer>, a distinct std::any type.
            return static_pointer_cast<Initializer>(
                make_shared<ArrayInitializer>(initializers, ctx->getStart()));
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
                    formalParameters.push_back(FormalParameter::fromContext(formalParameterContext, pModule));
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
            // v1: `@interface MyAnn { ... }` parses but is otherwise inert.
            // The annotation name is recognized when other code does
            // `@MyAnn` (Annotatable stores it by name), but the body's
            // element-method declarations aren't registered. Returning a
            // null `any` keeps onStructureDeclaration from trying to cast
            // a non-class result to CajetaClassPtr.
            return std::any(nullptr);
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

        virtual std::any visitRequiresModifier(CajetaParser::RequiresModifierContext* ctx) override {
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
                CajetaType::fromContext(ctx->typeType(), pModule),
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
            // Expression::fromContext builds the full sub-tree (including children) via
            // its own recursive descent; no further visitor-driven addChild loop is
            // needed, and the loop that used to live here aggregated visitChildren in a
            // way that broke for postfix operators.
            return Expression::fromContext(ctx);
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
        visitExplicitTemplateInvocation(CajetaParser::ExplicitTemplateInvocationContext* ctx) override {
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
            return CajetaType::fromContext(ctx, pModule);
        }

        virtual std::any visitPrimitiveType(CajetaParser::PrimitiveTypeContext* ctx) override {
            return CajetaType::fromContext(ctx, pModule);
        }

        virtual std::any visitTypeArguments(CajetaParser::TypeArgumentsContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitSuperSuffix(CajetaParser::SuperSuffixContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any
        visitExplicitTemplateInvocationSuffix(CajetaParser::ExplicitTemplateInvocationSuffixContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitArguments(CajetaParser::ArgumentsContext* ctx) override {
            return visitChildren(ctx);
        }
    };
}  // namespace code
