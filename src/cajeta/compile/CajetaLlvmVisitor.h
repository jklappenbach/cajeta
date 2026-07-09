
// Generated of /Users/julian/code/cpp/code/antlr4/CajetaParser.g4 by ANTLR 4.9.3

#pragma once


#include "antlr4-runtime.h"
#include "CajetaParserVisitor.h"
#include "CajetaLexer.h"
#include "cajeta/synth/SourceSynthesis.h"
#include "cajeta/synth/SourceSynthesisParse.h"
#include "cajeta/synth/SynthesizerRegistry.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaArray.h"
#include "cajeta/type/CajetaView.h"
#include "cajeta/type/CajetaView.h"
#include <any>
#include <unordered_set>
#include "cajeta/asn/Block.h"
#include "cajeta/asn/Statement.h"
#include "cajeta/asn/expression/Expression.h"
#include "cajeta/asn/LocalVariableDeclaration.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/asn/ClassBodyDeclaration.h"
#include "cajeta/asn/AnnotationParser.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/error/Exception.h"
#include "cajeta/error/Diagnostics.h"


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

        // Run every registered member synthesizer that claims `structure`
        // (source-synthesis facility, spec §3). Each returns a `{ ... }` class-
        // body fragment + the short-name imports it needs; we inject the imports
        // (only-when-unbound), parse the fragment, and reparent each member onto
        // the target — the field then flows through the normal pipeline
        // (FieldDeclaration -> StructureProperty -> generateStaticInitializers).
        // The parse pipeline is leaked on purpose (the AST holds token pointers
        // later clinit codegen dereferences). Members compose (several
        // synthesizers may inject into one class); a name that collides with an
        // existing or already-synthesized member is a loud error, not
        // last-writer-wins. A synthesizer that validates-first and rejects throws
        // through collectMembers as a user-attributed compile error.
        // `@Logged` is now a registered member synthesizer (see
        // registerBuiltinSynthesizers); this replaced the hard-wired
        // synthesizeLoggerField.
        void runMemberSynthesizers(CajetaClassPtr structure) {
            if (!structure) return;
            cajeta::synth::registerBuiltinSynthesizers();
            cajeta::synth::SynthesisContext ctx;
            ctx.parent = structure;
            ctx.module = pModule;
            auto claimed = cajeta::synth::SynthesizerRegistry::instance()
                .collectMembers(ctx);
            if (claimed.empty()) return;
            std::set<std::string> seen;
            for (auto& p : structure->getPropertyList()) {
                if (p) seen.insert(p->getName());
            }
            for (auto& [label, res] : claimed) {
                for (auto& imp : res.imports) {
                    cajeta::synth::injectImportIfUnbound(pModule, imp.first, imp.second);
                }
                auto* body = cajeta::synth::parseClassBodyFragment(res.classBodyFragment);
                for (auto* cbd : body->classBodyDeclaration()) {
                    MemberDeclarationPtr mem;
                    try {
                        mem = std::any_cast<MemberDeclarationPtr>(
                            visitClassBodyDeclaration(cbd));
                    } catch (...) { continue; }
                    auto fieldDecl =
                        std::dynamic_pointer_cast<FieldDeclaration>(mem);
                    if (!fieldDecl) continue;
                    std::size_t before = structure->getPropertyList().size();
                    fieldDecl->updateParent(structure);
                    auto& plist = structure->getPropertyList();
                    auto it = plist.begin();
                    std::advance(it, before);
                    for (; it != plist.end(); ++it) {
                        if (*it && !seen.insert((*it)->getName()).second) {
                            throw Exception(
                                "source-synthesis member collision: synthesizer '"
                                    + label + "' injects member '" + (*it)->getName()
                                    + "' which already exists on "
                                    + structure->getQName()->toCanonical()
                                    + " — no last-writer-wins",
                                "CAJETA_ERROR_SYNTH_MEMBER_COLLISION");
                        }
                    }
                }
            }
        }

        // Records have structural equality (records-spec §2.5.3): synthesize a
        // static field-wise `operator==` when the user didn't declare one.
        // Same fragment-parse pipeline as synthesizeLoggerField (and the same
        // deliberate ANTLR-pipeline leak — the AST holds token pointers).
        // Nested record fields are flattened to primitive-leaf compares
        // (`a.i.x == b.i.x`) rather than dispatching the inner operator== —
        // a value-type FIELD passed as a by-value operator arg currently
        // marshals as a pointer and trips the JIT verifier. `!=` derives
        // automatically.
        // Parents of a class-like during the visit pass: superClasses when
        // already resolved, else the declared extends names via canonicalMap
        // (resolveSuperClasses hasn't run yet at synthesis time).
        static std::vector<CajetaClassPtr> resolvedParentsOf(
                const CajetaClassPtr& cls) {
            std::vector<CajetaClassPtr> out;
            if (!cls) return out;
            if (!cls->getSuperClasses().empty()) {
                for (auto& s : cls->getSuperClasses()) if (s) out.push_back(s);
                return out;
            }
            auto& cmap = CajetaType::getCanonicalMap();
            for (auto& qn : cls->getQExtended()) {
                if (!qn) continue;
                auto it = cmap.find(qn->toCanonical());
                if (it == cmap.end()) it = cmap.find(qn->getTypeName());
                if (it == cmap.end() || !it->second) continue;
                if (auto c = std::dynamic_pointer_cast<CajetaClass>(it->second)) {
                    out.push_back(c);
                }
            }
            return out;
        }

        void appendRecordFieldCompares(const string& pathA, const string& pathB,
                const CajetaClassPtr& cls, string& expr) {
            // Inherited record fields first — mirrors the flat layout's
            // ancestor-prefix order; access paths stay flat (a.f resolves
            // inherited fields via the super walk).
            for (auto& sup : resolvedParentsOf(cls)) {
                appendRecordFieldCompares(pathA, pathB, sup, expr);
            }
            for (auto& prop : cls->getPropertyList()) {
                if (!prop || prop->isStatic()) continue;
                auto ft = prop->getType();
                // Inline array field (int8[12] etc.): expand per-element —
                // `==` on the array itself would pointer-compare the GEPs.
                if (auto arr = std::dynamic_pointer_cast<CajetaArray>(ft)) {
                    if (arr->isInlineArray()) {
                        for (int32_t i = 0; i < arr->getFixedLength(); ++i) {
                            if (!expr.empty()) expr += " && ";
                            string elem = "." + prop->getName()
                                + "[" + std::to_string(i) + "]";
                            expr += pathA + elem + " == " + pathB + elem;
                        }
                        continue;
                    }
                }
                auto fieldClass = std::dynamic_pointer_cast<CajetaClass>(ft);
                if (fieldClass && ft
                        && (ft->getTypeFlags() & VALUE_TYPE_FLAG)
                        && !fieldClass->getPropertyList().empty()) {
                    appendRecordFieldCompares(
                        pathA + "." + prop->getName(),
                        pathB + "." + prop->getName(), fieldClass, expr);
                    continue;
                }
                if (!expr.empty()) expr += " && ";
                expr += pathA + "." + prop->getName() + " == "
                    + pathB + "." + prop->getName();
            }
        }

        void synthesizeRecordEquality(CajetaClassPtr structure) {
            for (auto& kv : structure->getMethods()) {
                if (kv.second && kv.second->getName() == "operator==") return;
            }
            string typeName = structure->getQName()->getTypeName();
            string expr;
            appendRecordFieldCompares("a", "b", structure, expr);
            if (expr.empty()) return;
            string src = "{ public static boolean operator== (" + typeName
                + " a, " + typeName + " b) { return " + expr + "; } }";
            auto* body = cajeta::synth::parseClassBodyFragment(src);
            for (auto* cbd : body->classBodyDeclaration()) {
                MemberDeclarationPtr mem;
                try {
                    mem = std::any_cast<MemberDeclarationPtr>(
                        visitClassBodyDeclaration(cbd));
                } catch (ReuseHazardAbort&) {
                    throw;  // reuse rollback must reach the compile driver
                } catch (...) { continue; }
                if (auto methodDecl =
                        std::dynamic_pointer_cast<MethodDeclaration>(mem)) {
                    methodDecl->updateParent(structure);
                }
            }
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
            return std::any(buildClassLike(ctx, ctx->identifier()->getText(),
                ctx->typeParameters(), ctx->EXTENDS(), ctx->IMPLEMENTS(),
                ctx->PERMITS(), ctx->typeList(), /*isRecord=*/false));
        }

        virtual std::any visitRecordDeclaration(CajetaParser::RecordDeclarationContext* ctx) override {
            return std::any(buildClassLike(ctx, ctx->identifier()->getText(),
                ctx->typeParameters(), ctx->EXTENDS(), ctx->IMPLEMENTS(),
                nullptr, ctx->typeList(), /*isRecord=*/true));
        }

        // Shared builder for class-like declarations (class / record).
        CajetaClassPtr buildClassLike(antlr4::ParserRuleContext* ctx,
                const string& name,
                CajetaParser::TypeParametersContext* typeParametersCtx,
                antlr4::tree::TerminalNode* extendsKw,
                antlr4::tree::TerminalNode* implementsKw,
                antlr4::tree::TerminalNode* permitsKw,
                const std::vector<CajetaParser::TypeListContext*>& typeLists,
                bool isRecord) {
            string packageAdj;
            for (auto& structure: pModule->getStructureStack()) {
                packageAdj.append(".");
                packageAdj.append(structure->getQName()->getTypeName());
            }
            QualifiedNamePtr qName = QualifiedName::getOrInsert(name, pModule->getQName()->getPackageName() + packageAdj);
            list<QualifiedNamePtr> qExtended;
            list<QualifiedNamePtr> qImplemented;
            // Type arguments per implements entry (parallel to qImplemented;
            // empty inner vector for non-templated interface references).
            // Captured here at parse time so consumers (e.g. @Encoding's
            // verifier) can read `implements Encoder<T>` and recover T
            // without re-parsing.
            list<vector<QualifiedNamePtr>> qImplementedTypeArgs;
            // Grammar: `(EXTENDS typeList)? (IMPLEMENTS typeList)? (PERMITS typeList)?`
            // ANTLR exposes typeLists in source order; match each to its
            // keyword by start-token index. Sealed-class PERMITS is parsed
            // but its typeList is ignored in v1 (GAP-11).
            auto kwIdx = [](antlr4::tree::TerminalNode* n) -> ssize_t {
                return n && n->getSymbol() ? (ssize_t) n->getSymbol()->getTokenIndex() : -1;
            };
            ssize_t extKw = kwIdx(extendsKw);
            ssize_t implKw = kwIdx(implementsKw);
            ssize_t permKw = kwIdx(permitsKw);
            for (auto* tl : typeLists) {
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
                    auto* coi = tt->classOrInterfaceType();
                    bucket->push_back(QualifiedName::fromContext(coi));
                    // Pull type args off the leaf identifier; multi-level
                    // qualified templates like `Outer<A>.Inner<B>` aren't
                    // supported in v1 (would need per-level capture and
                    // a way to associate them with each identifier in the
                    // qName). For the common `Encoder<T>` shape, the leaf
                    // is the only identifier and the args land here.
                    if (which == 1) {
                        vector<QualifiedNamePtr> args;
                        // ANTLR's classOrInterfaceType exposes typeArguments
                        // as a vector (one slot per identifier in the dotted
                        // chain); the leaf's is the last non-null entry.
                        auto targsList = coi->typeArguments();
                        CajetaParser::TypeArgumentsContext* leafTargs = nullptr;
                        for (auto* ta : targsList) {
                            if (ta) leafTargs = ta;
                        }
                        if (leafTargs) {
                            for (auto* targ : leafTargs->typeArgument()) {
                                // Wildcards (`? extends Foo`) still land
                                // in v2; class-or-interface and primitive
                                // typeType are handled below.
                                if (!targ || !targ->typeType()) continue;
                                if (auto* targCoi = targ->typeType()
                                        ->classOrInterfaceType()) {
                                    args.push_back(
                                        QualifiedName::fromContext(targCoi));
                                } else if (auto* targPrim = targ->typeType()
                                        ->primitiveType()) {
                                    // Primitive-typed arg: build a
                                    // QualifiedName with no package
                                    // (primitives live in
                                    // CAJETA_NATIVE_PACKAGE = "").
                                    // resolveImplementedInterfaces's
                                    // canonMap lookup finds the registered
                                    // primitive by its short name.
                                    args.push_back(
                                        QualifiedName::getOrInsert(
                                            targPrim->getText(), ""));
                                }
                            }
                        }
                        qImplementedTypeArgs.push_back(std::move(args));
                    }
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
            structure->setQImplementedTypeArgs(std::move(qImplementedTypeArgs));

            // record = @ValueType final class with no vtable. Synthesizing the
            // annotation routes records through every existing @ValueType path
            // (flags in generatePrototype, POD validation below, template
            // annotation-carry at instantiation) with no parallel machinery.
            if (isRecord) {
                // Interface dispatch needs a vtable/itable; records have
                // neither (records-spec §2.5.4). Runs for templates too —
                // the gate fires at declaration, before any instantiation.
                if (!qImplemented.empty()) {
                    throw Exception(
                        "record '" + qName->toCanonical()
                            + "' cannot implement interface '"
                            + qImplemented.front()->toCanonical()
                            + "' — interface dispatch needs a vtable and "
                              "records have none; use a class",
                        "CAJETA_ERROR_RECORD_IMPLEMENTS");
                }
                structure->setRecordType(true);
                structure->addModifier(FINAL);
                if (!structure->findAnnotation("ValueType")) {
                    structure->addAnnotationInstance(make_shared<AnnotationInstance>(
                        QualifiedName::getOrInsert("ValueType", "")));
                }
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
            if (auto* tps = typeParametersCtx) {
                vector<TypeParameter> params;
                for (auto* tp : tps->typeParameter()) {
                    TypeParameter param(tp->identifier()->getText());
                    param.owningRequired = (tp->REFERENCE() != nullptr);  // element-ownership §4.1.5
                    if (auto* pt = tp->primitiveType()) {
                        // Non-type (integer-constant) parameter: `primitiveType identifier`.
                        param.isNonType = true;
                        param.nonTypePrimitive = pt->getText();
                    } else {
                        if (auto* bound = tp->typeBound()) {
                            for (auto* tt : bound->typeType()) {
                                if (auto* coi = tt->classOrInterfaceType()) {
                                    param.bounds.push_back(QualifiedName::fromContext(coi));
                                }
                            }
                        }
                        // Default type argument `<T = float32>`.
                        if (auto* dflt = tp->typeType()) {
                            param.defaultType = dflt->getText();
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
                    // Keyword modifiers (final / public / abstract / …) on the
                    // class declaration: capture onto the structure so codegen
                    // can ask `getModifiers()`. `final` in particular lets
                    // CajetaClass::invokeMethod devirtualize a final class's
                    // methods (no subclass can override them). A
                    // classOrInterfaceModifier is EITHER an annotation OR a
                    // keyword — annotation() is null for the keyword form.
                    if (!mod->annotation()) {
                        // `abstract` maps to Modifier::NONE, so gate on the
                        // raw keyword: an abstract record contradicts the
                        // no-vtable value model.
                        if (isRecord && mod->getText() == "abstract") {
                            throw Exception(
                                "record '" + qName->toCanonical()
                                    + "' cannot be abstract — records are "
                                      "concrete no-vtable value types",
                                "CAJETA_ERROR_RECORD_ABSTRACT");
                        }
                        Modifier m = Modifiable::toModifier(mod->getText());
                        if (m != NONE) structure->addModifier(m);
                        continue;
                    }
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

            // REFL-3.3 (decision D1): `@Sealed` bars reflective access to the
            // class's private members. Record it as the SEALED class modifier
            // so it both (a) rides into the RTTI header's modifiers word the
            // reflect API reads, and (b) is visible to the invoke/newInstance
            // adapter codegen, which omits private cases for a sealed class.
            if (structure->findAnnotation("Sealed")) {
                structure->addModifier(REFLECT_SEALED);
            }

            // REFL-8: `@Retained` keeps a class in the Class.forName registry
            // even when nothing statically references it. Advisory until AOT
            // stripping lands (every compiled class is registered today);
            // recorded as a class modifier so the future stripping pass and the
            // reflect API can both read it from the RTTI modifiers word.
            if (structure->findAnnotation("Retained")) {
                structure->addModifier(REFLECT_RETAINED);
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
            // (@ValueType: the VALUE_TYPE_FLAG is applied inside
            // CajetaClass::generatePrototype — after its typeFlags reset and before
            // the methods are prototyped — so the operator borrow check sees it. POD
            // validity is checked below once fields populate.)
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
            // Templates also register here: generatePrototype handles
            // template registration too, but tryGeneratePrototype defers
            // when a parent is still a placeholder (e.g. `SkipStream<T>
            // extends Stream<T>` parsed before Stream's own declaration).
            // Without this registration a later `heap SkipStream<T>(...)`
            // reference would fromContext-create a fresh placeholder that
            // nothing ever fills in — the original visitClassDeclaration
            // already ran. Symmetric with visitInterfaceDeclaration, which
            // calls generatePrototype unconditionally.
            CajetaType::getCanonicalMap()[qName->toCanonical()] =
                static_pointer_cast<CajetaType>(structure);
            CajetaType::getCanonicalMap()[qName->getTypeName()] =
                static_pointer_cast<CajetaType>(structure);

            // @GenerateMock: the generated `Mock<Name>` class name is pre-scanned
            // into the archive registry (Compiler.cpp ArchivePrescanVisitor) so a
            // forward reference resolves to a placeholder; CajetaClass::synthesizeMock
            // fills that placeholder during this target's generatePrototype.

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

                // Hash / equals + != / == consistency checks
                // (docs/OperatorOverloading.md §7). Skipped for
                // templates (whose body walk is skipped above —
                // re-runs at instantiation time).
                bool hasOpEq = false;
                bool hasOpNe = false;
                bool hasHash = false;
                // The methods map is keyed by canonical signature
                // (e.g. `Tag::operator!=(Tag,Tag)`), so match via the
                // method's bare name instead of the key.
                for (auto& kv : structure->getMethods()) {
                    const std::string& mname = kv.second->getName();
                    if (mname == "operator==") hasOpEq = true;
                    if (mname == "operator!=") hasOpNe = true;
                    if (mname == "hash")       hasHash = true;
                    // Records have no vtable: a body-less (abstract/virtual)
                    // method has nothing to dispatch through (records-spec
                    // §2.5.4). Template records re-check at instantiation.
                    if (isRecord && kv.second->isAbstract()) {
                        throw Exception(
                            "record '" + qName->toCanonical()
                                + "' declares abstract method '" + mname
                                + "' — records have no vtable; every record "
                                  "method needs a body",
                            "CAJETA_ERROR_RECORD_ABSTRACT_METHOD");
                    }
                }
                bool isObject = structure->getQName()->toCanonical()
                              == "cajeta.lang.Object";
                // @AutoHash (and @Data / @Value, which imply it) synthesize a
                // structural hash() — but only later, in tryGeneratePrototype's
                // synthesizeAutoHash(). This check runs before that, so treat
                // the annotation itself as satisfying hash().
                if (structure->findAnnotation("AutoHash")
                        || structure->findAnnotation("Data")
                        || structure->findAnnotation("Value")) {
                    hasHash = true;
                }
                // Reject `operator!=` declared without `operator==` —
                // != derives from == automatically. Standalone != is
                // almost always a bug (forgot to define == too).
                if (hasOpNe && !hasOpEq) {
                    char buf[400];
                    snprintf(buf, sizeof(buf),
                        "class '%s' declares operator!= but not operator==. "
                        "Cajeta derives `a != b` from `operator==` "
                        "automatically (returns the negation); standalone "
                        "operator!= is almost always a mistake. Fix: define "
                        "operator== too, and remove operator!= unless its "
                        "behavior genuinely differs from `!(a == b)`. "
                        "See docs/OperatorOverloading.md §7.",
                        structure->getQName()->toCanonical().c_str());
                    throw Exception(buf,
                        "CAJETA_ERROR_OPERATOR_NE_WITHOUT_EQ");
                }
                if (hasOpEq && !hasHash && !isObject) {
                    std::cerr << "warning: class '"
                              << structure->getQName()->toCanonical()
                              << "' defines operator== but does not override "
                                 "hash(). HashMap<this, V> will mis-key two "
                                 "==-equal instances with different identity "
                                 "hashes. Fix: add `@AutoHash` to the class "
                                 "to synthesize structural hash, or override "
                                 "hash() manually. See docs/"
                                 "OperatorOverloading.md §7. "
                                 "[CAJETA_WARN_HASH_EQUALS_MISMATCH]\n";
                }
                // @Factory discovery (AspectModel.md § @Factory, R1–R4).
                // Runs here (after the body walk, inside the non-template
                // guard) because it needs the provider methods populated;
                // templates re-run visitClassDeclaration per instantiation
                // (same reasoning as @Aspect). A @Factory class is ALSO
                // registered as a plain @Component so its @Inject
                // collaborators resolve and it is itself an injectable
                // singleton (R3's assisted case injects the factory).
                if (structure->findAnnotation("Factory")) {
                    CajetaModule::ComponentDescriptorPtr self;
                    for (auto& d : CajetaModule::getComponentClasses()) {
                        if (d && d->klass == structure) { self = d; break; }
                    }
                    if (!self) {
                        self = make_shared<CajetaModule::ComponentDescriptor>();
                        self->klass = structure;
                        CajetaModule::registerComponent(self);
                    }
                    auto fdesc = make_shared<CajetaModule::FactoryDescriptor>();
                    fdesc->klass = structure;
                    fdesc->selfComponent = self;
                    for (auto& kv : structure->getMethods()) {
                        auto& method = kv.second;
                        if (!method || method->isConstructor()) continue;
                        auto retType = method->getReturnType();
                        // A provider returns the product; void / null-return
                        // methods are helpers, not providers (R1).
                        if (!retType || retType->toCanonical() == "void") continue;
                        CajetaModule::FactoryProvider prov;
                        prov.method = method;
                        prov.providedType = retType;
                        // R4: method scope @Singleton / @Transient (Singleton default).
                        prov.scope = method->findAnnotation("Transient")
                            ? CajetaModule::AllocateMode::Transient
                            : CajetaModule::AllocateMode::Singleton;
                        // R1/R3: classify each param — @Inject = edge,
                        // unmarked = assisted (any assisted ⇒ factory-injection).
                        for (auto& p : method->getParameterList()) {
                            CajetaModule::FactoryProvider::Param fp;
                            fp.param = p;
                            if (auto inj = p->findAnnotation("Inject")) {
                                fp.injected = true;
                                fp.nameQualifier = inj->getString("name");
                            } else {
                                prov.hasAssisted = true;
                            }
                            prov.params.push_back(fp);
                        }
                        fdesc->providers.push_back(std::move(prov));
                    }
                    CajetaModule::registerFactory(fdesc);
                }
            }
            // Member synthesizers (@Logged, and instantiation-time member synth
            // like Table<T>) fire BEFORE the prototype is built, so injected
            // members are laid out and resolve as bare identifiers inside the
            // class's method bodies. Templates re-run visitClassDeclaration per
            // instantiation, so they synthesize then. Each synthesizer self-
            // selects (returns nullopt when it doesn't apply).
            runMemberSynthesizers(structure);
            // Template records synthesize per-instantiation (future work);
            // the template body walk is skipped so there are no fields here.
            if (isRecord && !structure->isTemplate()) {
                synthesizeRecordEquality(structure);
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
            // @ValueType (plans/value-type-overloading-plan.md): mark a by-value POD
            // class as a value type — eligible for operator-overload dispatch (the
            // !PRIMITIVE_FLAG gate is relaxed for VALUE_TYPE_FLAG) while still
            // marshalling by value through the existing POD path. Run AFTER
            // tryGeneratePrototype so fields are populated. Validate POD-ness (mirrors
            // isPodStruct, KernelArgTrait.cpp:86-99): no inherited fields, and every
            // non-static field a scalar primitive, a Vector (PRIMITIVE_FLAG), or
            // another @ValueType (VALUE_TYPE_FLAG — the recursion: that field's class
            // was validated when it was declared). On success OR the flag in.
            // (Non-template path only; a template @ValueType — e.g. Matrix<T,R,C> —
            // has placeholder fields and must validate at instantiation: future work.
            // Interfaces are rejected in visitInterfaceDeclaration.)
            if (structure->findAnnotation("ValueType")) {
                // A generic @ValueType template (e.g. Entry<K,V>) carries
                // placeholder field types that aren't yet known to be POD, so the
                // POD-ness check below can't run on the template — it validates at
                // each concrete instantiation instead (where K/V are bound). The
                // template still gets the value-type flags so its instantiations
                // and array storage are treated by value.
                if (!structure->isTemplate()) {
                    const string kind = isRecord ? "record" : "@ValueType class";
                    if (isRecord) {
                        // Static non-virtual inheritance (records-spec §2.6):
                        // every ancestor must itself be a record (Object, the
                        // fieldless auto-extend root, is exempt).
                        std::function<void(const CajetaClassPtr&)> checkAnc =
                            [&](const CajetaClassPtr& cls) {
                                for (auto& sup : cls->getSuperClasses()) {
                                    if (!sup) continue;
                                    auto qn = sup->getQName();
                                    bool isObj = qn
                                        && qn->getTypeName() == "Object"
                                        && qn->getPackageName() == "cajeta.lang";
                                    if (isObj) continue;
                                    if (!sup->isRecordType()) {
                                        throw Exception(
                                            "record '" + structure->toCanonical()
                                                + "' cannot extend '"
                                                + sup->toCanonical()
                                                + "' — records may only extend "
                                                  "records (static, non-virtual "
                                                  "composition)",
                                            "CAJETA_ERROR_RECORD_EXTENDS");
                                    }
                                    checkAnc(sup);
                                }
                            };
                        checkAnc(structure);
                        // 4.2.2 add-but-not-redefine: a derived record may not
                        // override/shadow an inherited instance method (static
                        // methods — operators, factories — dispatch on the
                        // static type and are exempt; so are constructors).
                        for (auto& kv : structure->getMethods()) {
                            auto& m = kv.second;
                            if (!m || m->isConstructor()) continue;
                            if (m->getModifiers().count(STATIC)) continue;
                            std::function<CajetaClassPtr(const CajetaClassPtr&)>
                                findShadowed = [&](const CajetaClassPtr& cls)
                                        -> CajetaClassPtr {
                                    for (auto& sup : cls->getSuperClasses()) {
                                        if (!sup || !sup->isRecordType()) continue;
                                        for (auto& skv : sup->getMethods()) {
                                            auto& sm = skv.second;
                                            if (!sm || sm->isConstructor()) continue;
                                            if (sm->getModifiers().count(STATIC)) continue;
                                            if (sm->getName() == m->getName()) {
                                                return sup;
                                            }
                                        }
                                        if (auto hit = findShadowed(sup)) return hit;
                                    }
                                    return nullptr;
                                };
                            if (auto owner = findShadowed(structure)) {
                                throw Exception(
                                    "record '" + structure->toCanonical()
                                        + "' method '" + m->getName()
                                        + "' overrides/shadows '"
                                        + owner->toCanonical() + "." + m->getName()
                                        + "' — record inheritance is "
                                          "add-but-not-redefine (overriding is "
                                          "the vtable line; use a class)",
                                    "CAJETA_ERROR_RECORD_OVERRIDE");
                            }
                        }
                    } else if (structure->countInheritedFields() != 0) {
                        throw Exception(
                            kind + " '" + structure->toCanonical()
                                + "' must not inherit fields — value types are flat POD",
                            "CAJETA_ERROR_VALUE_TYPE");
                    }
                    bool sawField = false;
                    for (auto& prop : structure->getPropertyList()) {
                        if (!prop || prop->isStatic()) continue;
                        sawField = true;
                        auto ft = prop->getType();
                        bool ok = ft
                            && (((ft->getTypeFlags() & PRIMITIVE_FLAG) != 0)
                                || ((ft->getTypeFlags() & VALUE_TYPE_FLAG) != 0));
                        if (!ok) {
                            throw Exception(
                                kind + " '" + structure->toCanonical()
                                    + "' field '" + prop->getName()
                                    + "' must be a value type (primitive, Vector, "
                                      "record, or @ValueType); '"
                                    + (ft && ft->getQName()
                                        ? ft->getQName()->toCanonical()
                                        : string("<unresolved>"))
                                    + "' is a reference (heap) type",
                                "CAJETA_ERROR_VALUE_TYPE");
                        }
                    }
                    if (!sawField
                            && !(isRecord && structure->countInheritedFields() != 0)) {
                        throw Exception(
                            kind + " '" + structure->toCanonical()
                                + "' must declare at least one field",
                            "CAJETA_ERROR_VALUE_TYPE");
                    }
                }
                // VALUE_TYPE_FLAG = the @ValueType kind (relaxes the operator-
                // dispatch !PRIMITIVE_FLAG gate); BY_VALUE_FLAG = the storage
                // axis (inline slot, Copy, no drop/borrow). Both born-correct on
                // cross-file placeholders too — see markArchiveValueType.
                structure->addTypeFlags(VALUE_TYPE_FLAG | BY_VALUE_FLAG);
            }
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

        virtual std::any visitViewDeclaration(CajetaParser::ViewDeclarationContext* ctx) override {
            // Zero-copy memory overlay (Views.md). Views are typed overlays
            // onto byte buffers (carved out of the unified-class model);
            // CajetaView inherits the legacy view-style codegen via
            // generatePrototypeImpl. Endianness/alignment annotations on
            // the enclosing typeDeclaration are read here.
            const string& name = ctx->identifier()->getText();
            string packageAdj;
            for (auto& structure : pModule->getStructureStack()) {
                packageAdj.append(".");
                packageAdj.append(structure->getQName()->getTypeName());
            }
            QualifiedNamePtr qName = QualifiedName::getOrInsert(
                name, pModule->getQName()->getPackageName() + packageAdj);
            auto viewStructure = make_shared<CajetaView>(pModule, qName);
            CajetaClassPtr structure = static_pointer_cast<CajetaClass>(viewStructure);

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

            // Placeholder reuse — mirror visitClassDeclaration (line 181).
            // A field/param/local of an earlier-parsed class that named this
            // interface created a forward-reference placeholder (via
            // CajetaType::fromContext's born-fat interface branch) and
            // captured its shared_ptr. Fill that SAME instance in place so
            // the interface's method set (and therefore its dispatch slots)
            // becomes visible through every earlier reference; otherwise a
            // freshly make_shared'd interface would orphan those references
            // at a methodless placeholder and `this.field.method()` dispatch
            // would find no slot and drop the call.
            //
            // Restricted to NON-GENERIC interfaces — exactly the set
            // fromContext's born-fat branch synthesizes a fat placeholder for
            // (a generic interface has typeParameters and is excluded there,
            // staying a thin class-shaped placeholder routed through the
            // template instantiation machinery). A generic interface
            // (`Encoder<T>`, `Stream<T>`) keeps its prior fresh-make_shared
            // path: reusing a generic placeholder here mis-seeds the template
            // (its `Encoder<X>` instantiation then fails the implements-
            // completeness check, CAJETA_ERROR_INTERFACE_NOT_IMPLEMENTED).
            bool isGenericIface = ctx->typeParameters() != nullptr;
            CajetaClassPtr interface;
            if (!isGenericIface) {
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
                        interface = existing;
                    }
                }
            }
            if (!interface) {
                interface = make_shared<CajetaClass>(
                    pModule, qName, qExtended, qImplemented);
            }
            interface->setIsInterface(true);
            // @ValueType is meaningless on an interface (value types are by-value
            // POD). The interface path never attaches annotations to the structure,
            // so check the enclosing typeDeclaration's modifiers directly and reject.
            if (auto* td = dynamic_cast<CajetaParser::TypeDeclarationContext*>(ctx->parent)) {
                for (auto* mod : td->classOrInterfaceModifier()) {
                    if (!mod->annotation()) continue;
                    auto inst = parseAnnotationInstance(mod->annotation());
                    if (inst && inst->getName()
                            && inst->getName()->getTypeName() == "ValueType") {
                        throw Exception(
                            "@ValueType cannot be applied to an interface",
                            "CAJETA_ERROR_VALUE_TYPE");
                    }
                }
            }

            // Templated interfaces (`interface Foo<T> { ... }`): mirror
            // the class-template handling at line 169. Capture the
            // type parameters so isTemplate() is true. The body walk
            // below then sees a template interface and skips body-
            // method emission, exactly like visitClassDeclaration's
            // `if (!structure->isTemplate())` guard at line 300 —
            // template bodies reference unresolved T placeholders that
            // can't lower until instantiation.
            //
            // For interfaces, instantiation lands when an implementing
            // class names `implements Foo<int32>`. Until that surface
            // matures, a templated interface lives as a placeholder
            // in canonicalMap; @Encoding's duck-typed dispatch path
            // sidesteps the issue by not requiring an instantiated
            // interface vtable.
            if (auto* tps = ctx->typeParameters()) {
                vector<TypeParameter> params;
                for (auto* tp : tps->typeParameter()) {
                    TypeParameter param(tp->identifier()->getText());
                    param.owningRequired = (tp->REFERENCE() != nullptr);  // element-ownership §4.1.5
                    if (auto* bound = tp->typeBound()) {
                        for (auto* tt : bound->typeType()) {
                            if (auto* coi = tt->classOrInterfaceType()) {
                                param.bounds.push_back(
                                    QualifiedName::fromContext(coi));
                            }
                        }
                    }
                    params.push_back(std::move(param));
                }
                interface->setTypeParameters(std::move(params));

                // Capture the full interfaceDeclaration source so
                // CajetaClass::instantiate can re-parse and walk the
                // body under an active type-parameter substitution.
                // Without this, templated-interface instantiation has
                // no source to revisit — analogous to the class-template
                // capture in visitClassDeclaration.
                antlr4::ParserRuleContext* enclosing = ctx;
                if (auto* td = dynamic_cast<CajetaParser::TypeDeclarationContext*>(ctx->parent)) {
                    enclosing = td;
                }
                auto* startTok = enclosing->getStart();
                auto* stopTok = enclosing->getStop();
                if (startTok && stopTok && startTok->getInputStream()) {
                    antlr4::misc::Interval interval(
                        startTok->getStartIndex(), stopTok->getStopIndex());
                    interface->setTemplateSource(
                        startTok->getInputStream()->getText(interval));
                }
            }

            pModule->getStructureStack().push_back(interface);

            // Build abstract Methods for each interfaceMethodDeclaration.
            // We sidestep visitClassBody/visitMethodDeclaration because those
            // expect a real method body; interface methods have either `;`
            // or a default block (the latter is deferred).
            //
            // Skip body emission for templated interfaces — same reason
            // class templates skip body walks (line 300): the body
            // references unresolved T placeholders. Implementing
            // classes will instantiate via their `implements Foo<X>`
            // clause once that path is fleshed out (currently
            // duck-typed via @Encoding).
            auto classBody = make_shared<ClassBodyDeclaration>(ctx->getStart());
            auto* body = ctx->interfaceBody();
            if (body && !interface->isTemplate()) {
                for (auto* bd : body->interfaceBodyDeclaration()) {
                    auto* md = bd->interfaceMemberDeclaration();
                    if (!md) continue;
                    auto* imd = md->interfaceMethodDeclaration();
                    if (!imd) continue;
                    auto* common = imd->interfaceCommonBodyDeclaration();
                    if (!common) continue;
                    // S9.4 — reject method-level generics on interface
                    // methods. The interface's vtable layout reserves
                    // one slot per method; method-level generics would
                    // need either monomorphization (requires knowing
                    // the type argument at declaration time) or
                    // dictionary-passing (out of scope for v1).
                    if (common->typeParameters()) {
                        char buf[320];
                        snprintf(buf, sizeof(buf),
                            "interface '%s' method '%s' declares its own "
                            "type parameters; method-level generics on "
                            "interface methods are not supported in v1 "
                            "(no place in the vtable for per-call template "
                            "instantiations to land).",
                            interface->getQName()->toCanonical().c_str(),
                            common->identifier()->getText().c_str());
                        throw Exception(buf,
                            "CAJETA_ERROR_INTERFACE_METHOD_GENERIC");
                    }
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


        virtual std::any visitClassBodyDeclaration(CajetaParser::ClassBodyDeclarationContext* ctx) override {
            // Nested class declaration: the grammar lists `classDeclaration`
            // as a memberDeclaration alternative. visitClassDeclaration
            // returns a CajetaClassPtr (already registered in canonicalMap
            // via the recursive call), which can't cast to MemberDeclarationPtr.
            // Wrap it in a no-op NestedClassDeclaration so the outer's
            // body iteration stays well-typed. The nested class itself is
            // an independent type whose qName carries the dotted outer-
            // path (visitClassDeclaration builds `packageAdj` from the
            // structureStack — see line 81).
            //
            // v1 supports static-nested only (no implicit outer-this);
            // grammar already allows STATIC on classDeclaration so users
            // get the Java-style "public static class Inner { ... }" form.
            if (auto memberDecl = ctx->memberDeclaration()) {
                if (memberDecl->classDeclaration()) {
                    std::any innerAny = visitClassDeclaration(memberDecl->classDeclaration());
                    CajetaClassPtr inner;
                    try {
                        inner = std::any_cast<CajetaClassPtr>(innerAny);
                    } catch (...) {
                        // If visitClassDeclaration's return shape ever
                        // changes, fall through to a null wrapper.
                    }
                    return std::static_pointer_cast<MemberDeclaration>(
                        std::make_shared<NestedClassDeclaration>(
                            inner, ctx->getStart()));
                }
                // Same routing for nested interfaces / enums / annotation
                // types — currently unsupported but keep them from
                // crashing the any_cast.
                if (memberDecl->interfaceDeclaration()
                        || memberDecl->enumDeclaration()
                        || memberDecl->annotationTypeDeclaration()) {
                    // Visit so the type-system catches what it can; wrap
                    // the result in the same no-op shape.
                    visitChildren(memberDecl);
                    return std::static_pointer_cast<MemberDeclaration>(
                        std::make_shared<NestedClassDeclaration>(
                            nullptr, ctx->getStart()));
                }
            }
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

            // Method-level template post-check (docs/specification/
            // MethodLevelTemplate.md): a declaration that introduces
            // method-level type parameters MUST be declared `final` or
            // `static`. The rule surfaces the non-virtuality at the
            // declaration site (the templating itself excludes the
            // method from the vtable, but readers benefit from the
            // explicit marker; same convention Java/C++ use). Also
            // capture the enclosing classBodyDeclaration's source text
            // here so per-call monomorphization can re-parse the
            // method with substitutions pushed without needing to
            // retain ANTLR contexts.
            // Operator-overload post-check (docs/OperatorOverloading.md §1).
            // Enforces the per-category staticness + arity rules AFTER
            // the modifier walk above has stamped STATIC onto the
            // method's modifier set. Per §1:
            //   - static (1 or 2 params):  + -
            //   - static (exactly 2):      * / % == != < > <= >= & | ^
            //   - static (exactly 1):      ! ~
            //   - instance (0 params):     ++ --
            //   - instance (1 param):      []
            //   - instance (2 params):     []=
            //   - instance (1 param):      += -= *= /= %= &= |= ^= <<= >>= >>>=
            // Diagnostics surface the exact Fix-It from §11.
            if (auto methodDecl = std::dynamic_pointer_cast<MethodDeclaration>(memberDeclaration)) {
                if (auto m = methodDecl->getMethod()) {
                    const std::string& name = m->getName();
                    bool isStatic = m->getModifiers().find(STATIC)
                                  != m->getModifiers().end();
                    size_t arity = m->getParameterList().size();

                    // Category 1: must-be-static operators. Map maps the
                    // operator name → (minArity, maxArity).
                    static const std::unordered_map<std::string, std::pair<size_t, size_t>>
                        kStaticOps = {
                            {"operator+",  {1, 2}}, {"operator-",  {1, 2}},
                            {"operator*",  {2, 2}}, {"operator/",  {2, 2}}, {"operator%",  {2, 2}},
                            {"operator==", {2, 2}}, {"operator!=", {2, 2}},
                            {"operator<",  {2, 2}}, {"operator>",  {2, 2}},
                            {"operator<=", {2, 2}}, {"operator>=", {2, 2}},
                            {"operator&",  {2, 2}}, {"operator|",  {2, 2}}, {"operator^",  {2, 2}},
                            {"operator!",  {1, 1}}, {"operator~",  {1, 1}},
                        };
                    // Category 2: must-be-instance operators. Map maps
                    // the operator name → expected param count.
                    static const std::unordered_map<std::string, size_t>
                        kInstanceOps = {
                            {"operator++",  0}, {"operator--",  0},
                            {"operator[]",  1}, {"operator[]=", 2},
                            {"operator+=", 1}, {"operator-=", 1}, {"operator*=", 1},
                            {"operator/=", 1}, {"operator%=", 1},
                            {"operator&=", 1}, {"operator|=", 1}, {"operator^=", 1},
                            {"operator<<=", 1}, {"operator>>=", 1}, {"operator>>>=", 1},
                        };

                    auto itStatic = kStaticOps.find(name);
                    if (itStatic != kStaticOps.end()) {
                        size_t lo = itStatic->second.first;
                        size_t hi = itStatic->second.second;
                        bool arityOk = arity >= lo && arity <= hi;
                        if (!isStatic || !arityOk) {
                            char buf[512];
                            const char* arityHint = (lo == hi)
                                ? (lo == 1 ? "one parameter" : "two parameters (LHS, RHS)")
                                : "one or two parameters (unary or binary form)";
                            snprintf(buf, sizeof(buf),
                                "'%s' must be declared `public static` with "
                                "%s. Cajeta's binary / non-mutating-unary "
                                "operator overloads are static — both "
                                "operands are explicit, neither is mutated, "
                                "the return is a fresh value. See "
                                "docs/OperatorOverloading.md §2-3. "
                                "Fix: rewrite as `public static T %s (...)`.",
                                name.c_str(), arityHint, name.c_str());
                            throw Exception(buf,
                                "CAJETA_ERROR_OPERATOR_NOT_STATIC");
                        }
                    }
                    auto itInstance = kInstanceOps.find(name);
                    if (itInstance != kInstanceOps.end()) {
                        size_t expected = itInstance->second;
                        if (isStatic || arity != expected) {
                            char buf[512];
                            snprintf(buf, sizeof(buf),
                                "'%s' must be declared as an instance method "
                                "(no `static` modifier) with %zu parameter%s. "
                                "Mutating unary, indexed access, and compound "
                                "assignment operators have a privileged "
                                "receiver — the host IS the target. See "
                                "docs/OperatorOverloading.md §§4-6.",
                                name.c_str(), expected,
                                expected == 1 ? "" : "s");
                            throw Exception(buf,
                                "CAJETA_ERROR_OPERATOR_NOT_INSTANCE");
                        }
                        // S3 (value-type-overloading-plan, Decision #3): a
                        // @ValueType class may NOT declare an instance MUTATING
                        // operator. Value types are by-value Copy — the receiver
                        // is a fresh copy, so an in-place mutation through `this`
                        // (operator++/--, operator[]=, compound-assign) would
                        // write the copy and silently lose the result. Read-only
                        // operator[] is exempt (returns a value, mutates nothing).
                        // Forbidding the DECLARATION closes the hole at the source:
                        // no value-type instance can then dispatch such an operator.
                        static const std::unordered_set<std::string> kMutatingOps = {
                            "operator++", "operator--", "operator[]=",
                            "operator+=", "operator-=", "operator*=",
                            "operator/=", "operator%=", "operator&=",
                            "operator|=", "operator^=", "operator<<=",
                            "operator>>=", "operator>>>=",
                        };
                        if (kMutatingOps.count(name)) {
                            auto enclosing = m->getParent();
                            if (enclosing && enclosing->findAnnotation("ValueType")) {
                                char buf[512];
                                snprintf(buf, sizeof(buf),
                                    "@ValueType class '%s' cannot declare the "
                                    "mutating operator '%s'. Value types are "
                                    "by-value (Copy) — an in-place mutation "
                                    "through the receiver writes a copy and is "
                                    "lost. Fix: model the change as a static "
                                    "operator returning a fresh value (e.g. "
                                    "`%s` -> a static op or a method returning a "
                                    "new instance). Read-only `operator[]` is "
                                    "allowed. See docs/"
                                    "OperatorOverloading.md and "
                                    "plans/value-type-overloading-plan.md.",
                                    enclosing->toCanonical().c_str(),
                                    name.c_str(), name.c_str());
                                throw Exception(buf,
                                    "CAJETA_ERROR_VALUE_TYPE_MUTATING_OPERATOR");
                            }
                        }
                    }
                    if (m->isMethodTemplate()) {
                        auto& mods = m->getModifiers();
                        bool isFinal = mods.find(FINAL) != mods.end();
                        bool isStatic = mods.find(STATIC) != mods.end();
                        if (!isFinal && !isStatic) {
                            char buf[400];
                            snprintf(buf, sizeof(buf),
                                "method '%s' introduces method-level type "
                                "parameter(s) but is not declared 'final' or "
                                "'static'. Method-level templates are non-"
                                "virtual (they occupy no vtable slot) and must "
                                "be marked explicitly to surface that property "
                                "at the declaration site. See docs/"
                                "specification/lang/templates/MethodLevelTemplate.md. Fix: add "
                                "'final' modifier (or 'static' if no receiver "
                                "is needed).",
                                m->getName().c_str());
                            throw Exception(buf,
                                "CAJETA_ERROR_METHOD_TEMPLATE_NOT_FINAL");
                        }
                        auto* startTok = ctx->getStart();
                        auto* stopTok = ctx->getStop();
                        if (startTok && stopTok && startTok->getInputStream()) {
                            antlr4::misc::Interval interval(
                                startTok->getStartIndex(), stopTok->getStopIndex());
                            m->setMethodSource(
                                startTok->getInputStream()->getText(interval));
                        }
                    }
                }
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
            // All operator-overload alternatives now use `typeTypeOrVoid`
            // for the return slot (see CajetaParser.g4 §
            // operatorOverloadDeclaration), so each operator can declare
            // a concrete return, `#T` ownership transfer, or `void`.
            CajetaTypePtr returnType;
            if (ctx->typeTypeOrVoid()) {
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
                // .back() = innermost class on the stack. For top-
                // level classes that's identical to .front(); for
                // nested classes (a methodDeclaration inside a
                // nested classBody) .back() is the immediately
                // enclosing class, which is the correct parent.
                pModule->getStructureStack().back());
            // `#T operator+ (...)` — return transfers ownership. With
            // the unified typeTypeOrVoid grammar, REFERENCE lives
            // inside the typeTypeOrVoid subtree for every alternative.
            if (ctx->typeTypeOrVoid()
                    && ctx->typeTypeOrVoid()->REFERENCE() != nullptr) {
                method->setReturnsOwnership(true);
            }
            return static_pointer_cast<MemberDeclaration>(
                make_shared<MethodDeclaration>(method, ctx->getStart()));
        }

        virtual std::any visitMethodDeclaration(CajetaParser::MethodDeclarationContext* ctx) override {
            string name = ctx->identifier()->getText();

            // Method-level templates (docs/specification/lang/templates/MethodLevelTemplate.md):
            // capture <R, ...> if present, push a placeholder substitution so
            // formals + return type referencing R resolve cleanly during this
            // pass, then capture the body source for per-call re-parse instead
            // of walking it (the body's locals reference the placeholder T-vars
            // and can't codegen without real arg types). The placeholder is a
            // lightweight CajetaClass whose canonical IS the type-parameter
            // name — same approach the class-template path uses while the
            // template snippet is being walked at instantiation time.
            vector<TypeParameter> methodTypeParameters;
            bool isMethodTemplate = false;
            if (auto* tps = ctx->typeParameters()) {
                isMethodTemplate = true;
                for (auto* tp : tps->typeParameter()) {
                    TypeParameter param(tp->identifier()->getText());
                    param.owningRequired = (tp->REFERENCE() != nullptr);  // element-ownership §4.1.5
                    if (auto* pt = tp->primitiveType()) {
                        // Non-type (integer-constant) method parameter:
                        // `primitiveType identifier` (e.g. `<uint32 N>`).
                        // Mirrors the class-level capture in Compiler.cpp.
                        param.isNonType = true;
                        param.nonTypePrimitive = pt->getText();
                    } else if (auto* bound = tp->typeBound()) {
                        for (auto* tt : bound->typeType()) {
                            if (auto* coi = tt->classOrInterfaceType()) {
                                param.bounds.push_back(QualifiedName::fromContext(coi));
                            }
                        }
                    }
                    methodTypeParameters.push_back(std::move(param));
                }
            }
            // Push placeholder substitution: each method-level T-var gets a
            // fresh CajetaClass placeholder named after the parameter. Real
            // instantiations replace this with concrete arg types.
            //
            // Discriminator: if the FIRST type-param name already resolves
            // via the current substitution stack, we're inside a re-parse
            // for monomorphization (MethodTemplateInstantiator pushed the
            // real types before walking). Don't shadow those bindings with
            // placeholders, and DO walk the body — that's the whole point
            // of the re-parse.
            bool isInstantiationReparse = false;
            if (isMethodTemplate && !methodTypeParameters.empty()) {
                if (pModule->lookupTypeParameter(
                        methodTypeParameters[0].name)) {
                    isInstantiationReparse = true;
                }
            }
            if (isMethodTemplate && !isInstantiationReparse) {
                // Start from any class-level substitution already in scope
                // (when this method lives inside a templated class being
                // instantiated, the class's T-vars are bound by the outer
                // push). Add the method-level placeholders ON TOP, then
                // push as a single frame — lookupTypeParameter only checks
                // the top frame, so we have to carry the inherited bindings
                // forward or class-level T-vars would fail to resolve in
                // the method's formals/return.
                std::map<std::string, CajetaTypePtr> ph;
                if (auto inherited = pModule->getCurrentTypeSubstitution()) {
                    ph = *inherited;
                }
                for (auto& tp : methodTypeParameters) {
                    auto qn = QualifiedName::getOrInsert(tp.name, "");
                    auto holder = std::make_shared<CajetaClass>(pModule, qn,
                        std::list<QualifiedNamePtr>{});
                    holder->setPlaceholder(true);
                    ph[tp.name] = holder;
                }
                pModule->pushTypeSubstitution(std::move(ph));
            }

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
            // methodBody is either `block` or `;` (abstract methods, interface
            // body methods). For the `;` form, visitMethodBody returns an
            // empty std::any and any_cast<BlockPtr> would throw bad_any_cast.
            // Guard so abstract methods land as Method with a null block.
            //
            // Method-template body parse is DEFERRED on the initial walk
            // (the body references method-level T-vars that can't codegen
            // without real arg types). On the re-parse path triggered by
            // MethodTemplateInstantiator, real arg types are bound — walk
            // the body normally.
            BlockPtr block;
            bool walkBody = !isMethodTemplate || isInstantiationReparse;
            if (walkBody && ctx->methodBody() && ctx->methodBody()->block()) {
                block = any_cast<BlockPtr>(visitMethodBody(ctx->methodBody()));
            }
            if (isMethodTemplate && !isInstantiationReparse) {
                pModule->popTypeSubstitution();
            }
            MethodPtr method = Method::create(
                this->pModule,
                name,
                returnType,
                formalParameters,
                block,
                // .back() (innermost class) — correct parent for
                // methods inside nested classes. For top-level
                // classes the stack has one entry and .back() ==
                // .front().
                pModule->getStructureStack().back());
            if (isMethodTemplate) {
                method->setMethodTypeParameters(std::move(methodTypeParameters));
                // Source-text capture happens in visitClassBodyDeclaration
                // where the enclosing modifiers (final/static) are in scope.
            }
            method->setVarargs(varargs);
            // No body (methodBody was `;`) = abstract method. Method::generate*
            // already skips function emission when abstractFlag is set;
            // CajetaClass::buildVirtualTable also uses isAbstract() to gate
            // override-vs-introduce semantics. Without this flag the method
            // would land as an empty-body method (codegen-default zero
            // return) and silently mask missing overrides.
            //
            // Method-template declarations also land with `block` null
            // (body parse is deferred to instantiation). They are NOT
            // abstract — the body source is captured and each call site
            // produces a fully-defined monomorphized instantiation. Guard
            // the setAbstract call so templates don't trip the abstract-
            // method-implementation check in CajetaClass.
            if (!block && !isMethodTemplate) {
                method->setAbstract(true);
            }
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
        // constructor's identifier. See docs/specification/lang/MemoryModel.md §
        // Destructors.
        virtual std::any visitDestructorDeclaration(CajetaParser::DestructorDeclarationContext* ctx) override {
            string declaredName = ctx->identifier()->getText();
            auto enclosing = pModule->getStructureStack().back();
            string className = enclosing
                ? enclosing->getQName()->getTypeName()
                : string();
            // For a generic class the type name is the monomorphized form
            // (e.g. "Buffer<float32>"), but the destructor is written against
            // the base name — `~Buffer()`. Compare against the base, stripping
            // any `<...>` type arguments, so generic classes can declare a
            // destructor the same way non-generic ones do.
            string baseName = className;
            if (auto lt = baseName.find('<'); lt != string::npos) {
                baseName = baseName.substr(0, lt);
            }
            if (!baseName.empty() && declaredName != baseName) {
                throw Exception(
                    "destructor name `~" + declaredName + "` must match "
                    "the enclosing class name (expected `~" + baseName + "`)",
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
            // v1: `annotation MyAnn { ... }` body element-methods stay inert.
            // We DO register the annotation as a minimal interface-like type in
            // canonicalMap (name only — no layout/vtable/codegen) so it resolves
            // as a type token, e.g. `Class.classesAnnotated<@MyAnn>()`. Not added
            // to structures, so onStructureDeclaration/generatePrototype never
            // touch it; returning null keeps it off the codegen worklist.
            // Register the annotation as a minimal type in canonicalMap (name
            // only) so it resolves as a type token, e.g. classesAnnotated<@A>().
            // Flagged isAnnotation so buildPendingPrototypes skips it — it never
            // lands in the structure map, keeping it out of the type-based
            // pointcut discriminator (resolveAdviceMatches). Body elements inert.
            // Package "code" matches how a bare `@Ann` usage canonicalizes
            // (QualifiedName::fromContext → "code.Ann"), so the declared type and
            // the applied annotation unify — classesAnnotated<@Ann>() injects the
            // same canonical the runtime registry matches on.
            QualifiedNamePtr qName = QualifiedName::getOrInsert(
                ctx->identifier()->getText(), "code");
            list<QualifiedNamePtr> none;
            auto ann = make_shared<CajetaClass>(pModule, qName, none, none);
            ann->setIsAnnotation(true);
            auto& canon = CajetaType::getCanonicalMap();
            // Register ONLY under the canonical "code.<Name>" key. A bare `@Foo`
            // usage and classesAnnotated<@Foo>() both canonicalize to "code.Foo"
            // (QualifiedName::fromContext), and findAnnotation/advice match by
            // short name against per-class instances — none need a bare short-name
            // canonicalMap entry. Registering the short name here would CLOBBER a
            // real class of the same short name, making that class in type
            // position resolve to the layout-less annotation → SIGSEGV at
            // allocation (task #65).
            canon[qName->toCanonical()] = static_pointer_cast<CajetaType>(ann);
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
            // Fail loud on an explicit-but-unresolvable type. `var` decls have a
            // null typeType() (inference fills the type later), so guard on it —
            // only an explicit type that resolves to null is an error. Without
            // this, a null type flows into generateCode and SIGSEGVs at the first
            // deref (e.g. type->hasValueSemantics()). A stale name (a renamed
            // class still spelled the old way) should be a clean diagnostic.
            auto* typeCtx = ctx->typeType();
            CajetaTypePtr declType = CajetaType::fromContext(typeCtx, pModule);
            if (typeCtx != nullptr && !declType) {
                reportOrThrow(typeCtx->getStart(), "CAJETA_ERROR_UNRESOLVED_TYPE",
                    "unresolved type '" + typeCtx->getText()
                        + "' in local variable declaration");
                declType = CajetaType::error();  // recover: analysis continues
            }
            return static_pointer_cast<BlockStatement>(make_shared<LocalVariableDeclaration>(
                modifiers,
                declType,
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

        // Try-with-resources grammar rules removed 2026-05-20 —
        // destructors fire deterministically at scope exit, see
        // docs/specification/lang/MemoryModel.md § "No try-with-resources".

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

        // `arrayLiteral : '[' expressionList? ']'` (XPU launch dims). Like the
        // other expression-subtree rules, the AST is built by
        // Expression::fromContext / ArrayLiteralExpression, so this visitor
        // entry just descends — it isn't on the codegen path.
        virtual std::any visitArrayLiteral(CajetaParser::ArrayLiteralContext* ctx) override {
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

        virtual std::any visitArguments(CajetaParser::ArgumentsContext* ctx) override {
            return visitChildren(ctx);
        }
    };
}  // namespace code
