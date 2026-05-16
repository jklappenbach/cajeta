
// Generated of /Users/julian/code/cpp/code/antlr4/CajetaParser.g4 by ANTLR 4.9.3

#pragma once


#include "antlr4-runtime.h"
#include "CajetaParserVisitor.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaStruct.h"
#include "cajeta/type/CajetaView.h"
#include <any>
#include "cajeta/asn/Block.h"
#include "cajeta/asn/Statement.h"
#include "cajeta/asn/expression/Expression.h"
#include "cajeta/asn/LocalVariableDeclaration.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/asn/ClassBodyDeclaration.h"
#include "cajeta/error/Exception.h"


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
            // Auto-extend Object: every class without an explicit
            // `extends` clause implicitly inherits cajeta.lang.Object,
            // the universal root. Skip Object itself (would create a
            // self-cycle). Interfaces and enums go through separate
            // visitor paths and aren't affected. resolveSuperClasses's
            // placeholder fallback handles the case where Object hasn't
            // been parsed yet — the dependency closes once Object lands
            // in canonicalMap.
            bool isObjectItself =
                qName->getTypeName() == "Object" &&
                qName->getPackageName() == "cajeta.lang";
            if (qExtended.empty() && !isObjectItself) {
                qExtended.push_back(
                    QualifiedName::getOrInsert("Object", "cajeta.lang"));
            }
            // Placeholder reuse. If some earlier-parsed class held a
            // forward reference to this class (created via CajetaType
            // ::fromContext's miss path), the placeholder is already
            // in canonicalMap under our canonical or short name.
            // Reuse the same CajetaClass instance — fillFromDeclaration
            // assigns module/qName/extends/implements on the existing
            // shared_ptr so every earlier reference now points at the
            // fully-filled class.
            CajetaClassPtr structure;
            {
                auto& canon = CajetaType::getCanonicalMap();
                auto it = canon.find(qName->toCanonical());
                if (it == canon.end()) {
                    it = canon.find(qName->getTypeName());
                }
                if (it != canon.end()) {
                    auto existing = std::dynamic_pointer_cast<CajetaClass>(it->second);
                    if (existing && existing->isPlaceholder()) {
                        existing->fillFromDeclaration(
                            pModule, qName, qExtended, qImplemented);
                        structure = existing;
                    }
                }
            }
            if (!structure) {
                structure = make_shared<CajetaClass>(pModule, qName, qExtended, qImplemented);
            }

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
            // typeDeclaration (e.g. `@Component(name = "disk") public
            // class DiskPersister`). parseAnnotationInstance walks the
            // element-value-pair list and stores typed values on the
            // resulting AnnotationInstance — consumers read them via
            // Annotatable::findAnnotation. The by-name set is also
            // populated for the call sites that only need presence.
            // See AspectModel.md § Implementation roadmap A1.
            if (auto* typeDecl = dynamic_cast<CajetaParser::TypeDeclarationContext*>(ctx->parent)) {
                for (auto* mod : typeDecl->classOrInterfaceModifier()) {
                    if (auto inst = parseAnnotationInstance(mod->annotation())) {
                        structure->addAnnotationInstance(inst);
                        // @SuppressLint on a class declaration: derive
                        // the cached rule-ID list from the typed args
                        // so isLintSuppressed stays O(N) over a tiny
                        // vector rather than walking annotations each
                        // call.
                        if (inst->getName()->getTypeName() == "SuppressLint") {
                            for (auto& id : inst->getStringList()) {
                                structure->addSuppressedLint(id);
                            }
                            // Single-string form: SuppressLint("foo").
                            const string& single = inst->getString();
                            if (!single.empty()) structure->addSuppressedLint(single);
                        }
                    }
                }
            }

            pModule->getStructureStack().push_back(structure);
            // Aspect registration (AspectModel.md § A2). A class
            // annotated `@Aspect` joins the process-global aspect
            // registry, which A3's pointcut-matching pass walks at
            // codegen time to find advice candidates for each method.
            // The annotation itself was captured in lockstep above —
            // findAnnotation reads from the same AnnotationInstance
            // list. Templates aren't registered as aspects: an
            // `@Aspect class Box<T>` shape doesn't have a concrete
            // class to advise from until instantiation, and v1's
            // grammar tests have no shape that exercises that. When
            // an instantiation lands it'll register itself the same
            // way through this visit (template body re-parse runs
            // visitClassDeclaration on the instantiated class).
            if (structure->findAnnotation("Aspect")) {
                CajetaModule::registerAspectClass(structure);
            }
            // Component registration (AspectModel.md § A8). @Component
            // and @Repository are sibling annotations — both register
            // as ordinary DI participants. @TestComponent registers
            // the same shape but flips isTestComponent so the
            // resolver can override a same-type @Component during
            // test compilations and drop it otherwise. Profiles are
            // collected by reading every @Profile annotation on the
            // class (repeatable in spirit even if v1 only records
            // each occurrence once per AnnotationInstance) — empty
            // list = profile-neutral.
            {
                auto componentAnn = structure->findAnnotation("Component");
                auto repositoryAnn = structure->findAnnotation("Repository");
                auto testComponentAnn = structure->findAnnotation("TestComponent");
                if (componentAnn || repositoryAnn || testComponentAnn) {
                    auto desc = make_shared<CajetaModule::ComponentDescriptor>();
                    desc->klass = structure;
                    desc->isTestComponent = (testComponentAnn != nullptr);
                    auto primary = componentAnn
                        ? componentAnn
                        : (repositoryAnn ? repositoryAnn : testComponentAnn);
                    if (primary) {
                        desc->name = primary->getString("name");
                    }
                    for (auto& inst : structure->getAnnotationInstances()) {
                        if (inst && inst->getName()
                                && inst->getName()->getTypeName() == "Profile") {
                            const string& p = inst->getString();
                            if (!p.empty()) desc->profiles.push_back(p);
                        }
                    }
                    CajetaModule::registerComponent(desc);
                }
            }
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
            // tryGeneratePrototype is the deferred-aware variant: if any
            // superclass / implemented interface is still a placeholder
            // (forward reference whose declaration hasn't been parsed
            // yet), it returns false without touching the LLVM struct
            // body. CajetaModule::buildPendingPrototypes runs after every
            // module's parse completes and walks canonicalMap to
            // fixed-point, so deferred classes get prototyped once their
            // parents fill in.
            structure->tryGeneratePrototype();
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

        // Shared body for visitStructDeclaration and visitViewDeclaration.
        // S2: struct produces CajetaStruct (stub — generatePrototype throws),
        // view produces CajetaView (real codegen via generatePrototypeImpl).
        // Both share annotation parsing and module-registration plumbing.
        std::any buildStructOrViewNode(
                const string& name,
                antlr4::tree::ParseTree* parent,
                antlr4::ParserRuleContext* ctx,
                bool asView) {
            string packageAdj;
            for (auto& structure : pModule->getStructureStack()) {
                packageAdj.append(".");
                packageAdj.append(structure->getQName()->getTypeName());
            }
            QualifiedNamePtr qName = QualifiedName::getOrInsert(
                name, pModule->getQName()->getPackageName() + packageAdj);
            CajetaAggregatePtr structure = asView
                ? static_pointer_cast<CajetaAggregate>(make_shared<CajetaView>(pModule, qName))
                : static_pointer_cast<CajetaAggregate>(make_shared<CajetaStruct>(pModule, qName));

            // Endianness / alignment annotations apply only to views (per
            // Structs.md, structs are host-endian with compiler-chosen
            // layout; per Views.md, views require endianness and may opt
            // into natural alignment). The annotations sit on the enclosing
            // typeDeclaration's classOrInterfaceModifier* prefix.
            if (asView) {
                auto viewStructure = static_pointer_cast<CajetaView>(structure);
                if (auto* typeDecl = dynamic_cast<CajetaParser::TypeDeclarationContext*>(parent)) {
                    for (auto* mod : typeDecl->classOrInterfaceModifier()) {
                        auto* ann = mod->annotation();
                        if (!ann) continue;
                        string aName = ann->qualifiedName()
                            ? ann->qualifiedName()->getText()
                            : (ann->altAnnotationQualifiedName()
                                ? ann->altAnnotationQualifiedName()->getText()
                                : string());
                        if (aName == "BigEndian") {
                            viewStructure->setEndianness(ViewEndianness::Big);
                        } else if (aName == "LittleEndian") {
                            viewStructure->setEndianness(ViewEndianness::Little);
                        } else if (aName == "HostEndian") {
                            viewStructure->setEndianness(ViewEndianness::Host);
                        } else if (aName == "Align") {
                            viewStructure->setAlignment(ViewAlignment::Natural);
                        }
                    }
                }
            }

            // S9.1 — `struct Foo implements I1, I2 { ... }`. Parse the
            // implements clause from the struct's context (views don't
            // have implements per Views.md). Stored as qImplemented
            // QualifiedNames; resolved to actual CajetaInterface
            // instances during CajetaStruct::generatePrototype via
            // resolveImplementedInterfaces() so forward-refs work.
            if (!asView) {
                if (auto* structCtx = dynamic_cast<CajetaParser::StructDeclarationContext*>(ctx)) {
                    list<QualifiedNamePtr> qImpl;
                    if (auto* tl = structCtx->typeList()) {
                        for (auto& tt : tl->typeType()) {
                            qImpl.push_back(QualifiedName::fromContext(tt->classOrInterfaceType()));
                        }
                    }
                    if (!qImpl.empty()) {
                        structure->setQImplemented(std::move(qImpl));
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

        virtual std::any visitStructDeclaration(CajetaParser::StructDeclarationContext* ctx) override {
            // Stack value aggregate (Structs.md). S2 produces a CajetaStruct
            // whose generatePrototype throws CAJETA_ERROR_STRUCT_UNIMPLEMENTED
            // — real codegen lands S6.
            return buildStructOrViewNode(ctx->identifier()->getText(), ctx->parent, ctx, /*asView=*/false);
        }

        virtual std::any visitViewDeclaration(CajetaParser::ViewDeclarationContext* ctx) override {
            // Zero-copy memory overlay (Views.md). S2 routes to CajetaView,
            // which inherits the legacy view-style codegen via
            // generatePrototypeImpl. S3-S5 extend it (owning variant,
            // required endianness, multiple var-size fields, nested views).
            return buildStructOrViewNode(ctx->identifier()->getText(), ctx->parent, ctx, /*asView=*/true);
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

        // Function-type production: `(T1, T2) -> R`. Just delegates to
        // CajetaType::fromContext on the enclosing typeType. The actual
        // CajetaFunctionType is built there (the visitFunctionType entry
        // point exists only to satisfy the visitor's pure-virtual hook —
        // the typeType path is what callers use).
        virtual std::any visitFunctionType(CajetaParser::FunctionTypeContext* ctx) override {
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

        // Parse `@SuppressLint(...)`'s string argument(s). Accepts either
        // a single string literal (`"foo"`) or an array initializer
        // (`{"foo", "bar"}`). Stripped of whitespace, braces, and quotes;
        // each string token becomes a separate lint-rule ID. Escape
        // sequences inside the literals aren't supported (lint IDs are
        // ASCII kebab-case by convention — see LintRules.md).
        static void parseLintIds(const std::string& argText,
                                 std::vector<std::string>& out) {
            std::string current;
            bool inQuotes = false;
            for (char c : argText) {
                if (c == '"') {
                    if (inQuotes && !current.empty()) {
                        out.push_back(current);
                        current.clear();
                    }
                    inQuotes = !inQuotes;
                } else if (inQuotes) {
                    current.push_back(c);
                }
            }
        }

        // Strip surrounding ASCII whitespace from a literal's text.
        // Annotation argument tokens come from ANTLR's getText() which
        // concatenates token text verbatim — array initializers in
        // particular contain interior whitespace around the commas.
        static std::string trimWs(const std::string& s) {
            size_t b = 0, e = s.size();
            while (b < e && std::isspace((unsigned char) s[b])) ++b;
            while (e > b && std::isspace((unsigned char) s[e - 1])) --e;
            return s.substr(b, e - b);
        }

        // Classify a single element-value token text and write the
        // discriminated result into `out`. Recognized shapes:
        //   "foo"     → AnnotationArgKind::String,   strVal=foo
        //   123       → AnnotationArgKind::Int64,    i64Val=123
        //   true      → AnnotationArgKind::Bool,     boolVal=true
        //   Foo.class → AnnotationArgKind::ClassRef, strVal=Foo
        // Anything else falls through to String with the raw text —
        // the user gets back what they typed, which is enough for
        // the current annotation surface (no nested annotations
        // here; A2+ may add typed references). Returns true on a
        // confident classification.
        static bool classifyLiteral(const std::string& raw, AnnotationArg& out) {
            std::string t = trimWs(raw);
            if (t.empty()) {
                out.kind = AnnotationArgKind::String;
                out.strVal = "";
                return false;
            }
            if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
                out.kind = AnnotationArgKind::String;
                out.strVal = t.substr(1, t.size() - 2);
                return true;
            }
            if (t == "true" || t == "false") {
                out.kind = AnnotationArgKind::Bool;
                out.boolVal = (t == "true");
                return true;
            }
            // Class literal — `Foo.class`. The grammar's primary
            // production for `typeTypeOrVoid '.' CLASS` produces this
            // text. Strip the suffix and capture the type-name
            // prefix; pointcut matching (A3) then resolves it
            // against the registered classes. Qualified names
            // (`pkg.Foo.class`) keep the dots; the matcher looks up
            // by short name first, then canonical.
            {
                static const std::string suffix = ".class";
                if (t.size() > suffix.size()
                        && std::equal(suffix.rbegin(), suffix.rend(), t.rbegin())) {
                    out.kind = AnnotationArgKind::ClassRef;
                    out.strVal = t.substr(0, t.size() - suffix.size());
                    return true;
                }
            }
            // Integer (decimal, with optional leading sign). Hex/oct/
            // binary literals can land here later when annotations
            // actually use them.
            bool numeric = !t.empty();
            size_t i = 0;
            if (t[0] == '+' || t[0] == '-') ++i;
            if (i == t.size()) numeric = false;
            for (; i < t.size() && numeric; ++i) {
                if (!std::isdigit((unsigned char) t[i])) numeric = false;
            }
            if (numeric) {
                try {
                    out.kind = AnnotationArgKind::Int64;
                    out.i64Val = (int64_t) std::stoll(t);
                    return true;
                } catch (...) {
                    // Fall through to raw-string fallback.
                }
            }
            // Unknown shape (identifier reference, enum constant, etc.).
            // Keep as a raw string so consumers see the source text;
            // A2+ may parse these into typed references.
            out.kind = AnnotationArgKind::String;
            out.strVal = t;
            return false;
        }

        // Build an AnnotationInstance from an ANTLR annotation context.
        // Walks elementValuePairs (name = value, name = value, ...) or
        // a single bare elementValue (the unnamed-arg form, stored
        // with name="" and looked up by callers as the conventional
        // "value" key). Array initializers map to *List kinds; each
        // element classifies independently and the list takes the
        // dominant kind (all-strings → StringList, all-ints → Int64List,
        // all-bools → BoolList; mixed lists fall back to StringList of
        // the raw text). Unsupported elementValue shapes (nested
        // annotation, expressions beyond literals) are captured with
        // their raw getText() as a String; A2+ can refine.
        //
        // `ann` may be null — returns nullptr in that case. The
        // returned instance always has a resolvable name (or nullptr
        // if the annotation context didn't yield one, which is a
        // malformed parser state we don't try to recover from).
        static AnnotationInstancePtr parseAnnotationInstance(
                CajetaParser::AnnotationContext* ann) {
            if (!ann) return nullptr;
            QualifiedNamePtr qn;
            if (ann->qualifiedName()) {
                qn = QualifiedName::fromContext(ann->qualifiedName());
            } else if (auto* alt = ann->altAnnotationQualifiedName()) {
                // `pkg.@MyAnn` form — leaf identifier is the
                // annotation name. v1 takes the short name only;
                // package-qualified annotation lookups can be added
                // when reflection consumers need the full canonical.
                const auto& ids = alt->identifier();
                if (!ids.empty()) {
                    qn = QualifiedName::getOrInsert(
                        ids.back()->getText(), "");
                }
            }
            if (!qn) return nullptr;

            auto inst = std::make_shared<AnnotationInstance>(qn);

            // Helper to populate a single AnnotationArg from one
            // elementValue context. Recursively handles array
            // initializers by collecting child element-value texts.
            std::function<void(CajetaParser::ElementValueContext*,
                               AnnotationArg&)> readArg =
                [&](CajetaParser::ElementValueContext* ev, AnnotationArg& arg) {
                    if (!ev) return;
                    if (auto* arr = ev->elementValueArrayInitializer()) {
                        // Array — first pass classifies each child,
                        // second pass picks the dominant kind.
                        std::vector<AnnotationArg> parts;
                        for (auto* child : arr->elementValue()) {
                            AnnotationArg p;
                            readArg(child, p);
                            parts.push_back(std::move(p));
                        }
                        // Pick dominant kind. All-same wins; mixed
                        // collapses to StringList of the raw text.
                        bool allString = !parts.empty(), allInt = !parts.empty(), allBool = !parts.empty();
                        for (auto& p : parts) {
                            if (p.kind != AnnotationArgKind::String) allString = false;
                            if (p.kind != AnnotationArgKind::Int64)  allInt = false;
                            if (p.kind != AnnotationArgKind::Bool)   allBool = false;
                        }
                        if (allInt) {
                            arg.kind = AnnotationArgKind::Int64List;
                            for (auto& p : parts) arg.i64List.push_back(p.i64Val);
                        } else if (allBool) {
                            arg.kind = AnnotationArgKind::BoolList;
                            for (auto& p : parts) arg.boolList.push_back(p.boolVal);
                        } else {
                            arg.kind = AnnotationArgKind::StringList;
                            for (auto& p : parts) {
                                // For a homogeneous string array each
                                // p.strVal is already the unquoted
                                // payload; for mixed shapes, fall back
                                // to whatever classifyLiteral put in
                                // strVal (or stringify ints/bools).
                                if (p.kind == AnnotationArgKind::String) {
                                    arg.strList.push_back(p.strVal);
                                } else if (p.kind == AnnotationArgKind::Int64) {
                                    arg.strList.push_back(std::to_string(p.i64Val));
                                } else if (p.kind == AnnotationArgKind::Bool) {
                                    arg.strList.push_back(p.boolVal ? "true" : "false");
                                } else {
                                    arg.strList.push_back(p.strVal);
                                }
                            }
                        }
                        return;
                    }
                    if (auto* nested = ev->annotation()) {
                        // Nested annotation — captured as raw text for
                        // now; A2+ can promote to a real nested
                        // AnnotationInstance.
                        arg.kind = AnnotationArgKind::String;
                        arg.strVal = nested->getText();
                        return;
                    }
                    // expression — text-classify the leaf token.
                    classifyLiteral(ev->getText(), arg);
                };

            if (auto* evp = ann->elementValuePairs()) {
                // `@Foo(name = value, other = thing)`.
                for (auto* pair : evp->elementValuePair()) {
                    AnnotationArg arg;
                    if (pair->identifier()) {
                        arg.name = pair->identifier()->getText();
                    }
                    readArg(pair->elementValue(), arg);
                    inst->addArg(std::move(arg));
                }
            } else if (auto* ev = ann->elementValue()) {
                // `@Foo(value)` — single unnamed arg. Stored with
                // empty name; findArg("value") routes through to it.
                AnnotationArg arg;
                readArg(ev, arg);
                inst->addArg(std::move(arg));
            }
            // `@Foo` with no parens at all — empty args, the instance
            // still records the annotation by name.
            return inst;
        }

        virtual std::any visitClassBodyDeclaration(CajetaParser::ClassBodyDeclarationContext* ctx) override {
            MemberDeclarationPtr memberDeclaration = any_cast<MemberDeclarationPtr>(visitMemberDeclaration(
                ctx->memberDeclaration()));
            // Annotation capture for class-body members. Walks each
            // modifier looking for annotations, builds a typed
            // AnnotationInstance per occurrence, and attaches it to
            // the underlying Method. The cached @SuppressLint rule-
            // ID list is derived here from the captured args so the
            // hot-path isLintSuppressed check stays O(N) over a tiny
            // vector — same shape the class-level path uses.
            //
            // A8 extends field-side capture so @Inject(name=...,
            // allocate=...) on a field is observable by the DI graph.
            // Both branches share the same modifier walk; the body
            // dispatches based on what member shape we resolved.
            // Class-level annotations live on the CajetaClass via
            // visitClassDeclaration's separate capture loop.
            if (auto methodDecl = std::dynamic_pointer_cast<MethodDeclaration>(memberDeclaration)) {
                if (auto m = methodDecl->getMethod()) {
                    for (auto& modifierContext : ctx->modifier()) {
                        auto* coim = modifierContext->classOrInterfaceModifier();
                        if (!coim) continue;
                        if (auto inst = parseAnnotationInstance(coim->annotation())) {
                            m->addAnnotationInstance(inst);
                            if (inst->getName()->getTypeName() == "SuppressLint") {
                                for (auto& id : inst->getStringList()) {
                                    m->addSuppressedLint(id);
                                }
                                const std::string& single = inst->getString();
                                if (!single.empty()) m->addSuppressedLint(single);
                            }
                        }
                    }
                }
            } else if (auto fieldDecl = std::dynamic_pointer_cast<FieldDeclaration>(memberDeclaration)) {
                for (auto& modifierContext : ctx->modifier()) {
                    auto* coim = modifierContext->classOrInterfaceModifier();
                    if (!coim) continue;
                    if (auto inst = parseAnnotationInstance(coim->annotation())) {
                        fieldDecl->addAnnotationInstance(inst);
                    }
                }
            }
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
            // Bracket forms checked first — `OPERATOR LBRACK RBRACK
            // (ASSIGN)?` overlaps with the bare ASSIGN check below
            // (the indexed-assignment form has both LBRACK and ASSIGN
            // tokens present), so the more specific match wins.
            if (ctx->LBRACK() && ctx->RBRACK() && ctx->ASSIGN()) sym = "[]=";
            else if (ctx->LBRACK() && ctx->RBRACK()) sym = "[]";
            else if (ctx->ADD()) sym = "+";
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
            // Most operator overloads declare a non-void return via
            // bare `typeType`; the indexed-assignment form (operator[]=)
            // uses `typeTypeOrVoid` so `void` is acceptable as the
            // return. Prefer typeType when present (the existing form);
            // fall back to typeTypeOrVoid for the bracket-only path.
            CajetaTypePtr returnType;
            if (ctx->typeType()) {
                returnType = CajetaType::fromContext(ctx->typeType(), pModule);
            } else if (ctx->typeTypeOrVoid()) {
                returnType = CajetaType::fromContext(ctx->typeTypeOrVoid(), pModule);
            }
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
            // `throws T1, T2` — advisory list of RecoverableException
            // subtypes the body may produce. Carried on the Method for the
            // lint pass; no enforcement here. See ErrorModel.md.
            if (auto* qnList = ctx->qualifiedNameList()) {
                vector<QualifiedNamePtr> throws;
                for (auto* qn : qnList->qualifiedName()) {
                    throws.push_back(QualifiedName::fromContext(qn));
                }
                method->setThrowsList(std::move(throws));
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
            // Constructors can also declare `throws T1, T2` per the grammar.
            if (auto* qnList = ctx->qualifiedNameList()) {
                vector<QualifiedNamePtr> throws;
                for (auto* qn : qnList->qualifiedName()) {
                    throws.push_back(QualifiedName::fromContext(qn));
                }
                method->setThrowsList(std::move(throws));
            }
            return static_pointer_cast<MemberDeclaration>(make_shared<MethodDeclaration>(method, ctx->getStart()));
        }

        // `~ClassName() { ... }` — destructor declaration. Builds the
        // body as a method internally named "drop" so the existing
        // class-drop wrapper machinery (CajetaClass::getOrCreateDropFunction)
        // picks it up unchanged. The identifier between ~ and ( must
        // match the enclosing class name, same convention as the
        // constructor's identifier. See cajeta-docs/MemoryModel.md §
        // Destructors.
        virtual std::any visitDestructorDeclaration(CajetaParser::DestructorDeclarationContext* ctx) override {
            string declaredName = ctx->identifier()->getText();
            auto enclosing = pModule->getStructureStack().back();
            string className = enclosing
                ? enclosing->getQName()->getTypeName()
                : string();
            if (!className.empty() && declaredName != className) {
                throw Exception(
                    "destructor name `~" + declaredName + "` must match "
                    "the enclosing class name (expected `~" + className + "`)",
                    "CAJETA_ERROR_TYPE");
            }
            BlockPtr block = any_cast<BlockPtr>(visitBlock(ctx->destructorBody));
            // Internally a destructor IS the class's drop method.
            // The synthesized __cajeta_<class>_drop wrapper looks up a
            // method named "drop" (per Method::getName) and calls it
            // before freeing the instance.
            string dropName = "drop";
            vector<FormalParameterPtr> noParams;
            MethodPtr method = Method::create(pModule, dropName,
                CajetaType::of("void"),
                noParams,
                block,
                enclosing);
            return static_pointer_cast<MemberDeclaration>(make_shared<MethodDeclaration>(method, ctx->getStart()));
        }

        virtual std::any visitFieldDeclaration(CajetaParser::FieldDeclarationContext* ctx) override {
            CajetaTypePtr type = any_cast<CajetaTypePtr>(visitTypeType(ctx->typeType()));
            // Forward-reference tolerance: fromContext synthesizes a
            // placeholder CajetaClass when the named type is known
            // to the archive but hasn't been visited yet, and throws
            // CAJETA_ERROR_UNKNOWN_TYPE for names not declared
            // anywhere in the compilation unit. Either we got a real
            // type back (resolved or placeholder), or fromContext
            // already threw — no extra reject needed here. The
            // post-parse pass catches any placeholder left unfilled.
            if (!type) {
                // Belt-and-suspenders: should be unreachable now that
                // fromContext either succeeds or throws, but a null
                // here would still segfault generatePrototype, so
                // emit the same diagnostic shape as before.
                string typeName = ctx->typeType()->getText();
                string declared = ctx->variableDeclarators()->getText();
                throw Exception(
                    "unknown field type '" + typeName
                        + "' on declaration '" + declared
                        + "'; not a primitive, native, or user-defined type",
                    "CAJETA_ERROR_UNKNOWN_TYPE");
            }
            return static_pointer_cast<MemberDeclaration>(
                make_shared<FieldDeclaration>(
                type,
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

        virtual std::any
        visitAggregateInitializer(CajetaParser::AggregateInitializerContext* ctx) override {
            // S6.2 — `Foo { field: expr, ... }`. The AST node + codegen live
            // in AggregateInitializerExpression and are built lazily by
            // PrimaryExpression::fromContext; this visit just descends so
            // any nested expressions in the parameterList are walked too.
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
