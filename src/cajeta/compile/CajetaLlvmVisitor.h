
// Generated of /Users/julian/code/cpp/code/antlr4/CajetaParser.g4 by ANTLR 4.9.3

#pragma once


#include "antlr4-runtime.h"
#include "CajetaParserVisitor.h"
#include "CajetaLexer.h"
#include "cajeta/synth/SourceSynthesis.h"
#include "cajeta/synth/SourceSynthesisParse.h"
#include "cajeta/synth/SynthesizerRegistry.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/xref/XrefIndex.h"
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

    /** Walks the Cajeta parse tree, building the module's structures, methods and
     *  AST nodes. One visit* hook per grammar production: most simply descend, and
     *  the substantive ones are documented individually. */
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

        // Run every registered companion synthesizer that claims `structure`, parse
        // each emitted class, and register it as a REAL named type so ordinary source
        // can spell it. Memoized on the canonical name; a placeholder is FILLED.
        void runCompanionSynthesizers(CajetaClassPtr structure) {
            if (!structure) return;
            cajeta::synth::registerBuiltinSynthesizers();
            cajeta::synth::SynthesisContext cctx;
            cctx.parent = structure;
            cctx.module = pModule;
            auto companions = cajeta::synth::SynthesizerRegistry::instance()
                .collectCompanions(cctx);
            if (companions.empty()) return;
            auto& cmap = CajetaType::getCanonicalMap();
            for (auto& [label, res] : companions) {
                std::string canonical = res.packageName.empty()
                    ? res.className
                    : res.packageName + "." + res.className;
                CajetaClassPtr existing;
                {
                    auto it = cmap.find(canonical);
                    if (it == cmap.end()) it = cmap.find(res.className);
                    if (it != cmap.end()) {
                        existing = std::dynamic_pointer_cast<CajetaClass>(
                            it->second);
                        if (existing && !existing->isPlaceholder()) continue;
                        if (!existing) continue;
                    }
                }
                for (auto& imp : res.imports) {
                    if (CajetaModule::stdlibImportHook) {
                        CajetaModule::stdlibImportHook(imp.second);
                    }
                    cajeta::synth::injectImportIfUnbound(
                        pModule, imp.first, imp.second);
                }
                std::string source = (res.packageName.empty()
                        ? std::string()
                        : ("package " + res.packageName + ";\n"))
                    + res.classSource;
                auto* unit = cajeta::synth::parseSynthesizedUnit(source);
                CajetaParser::ClassDeclarationContext* classDecl = nullptr;
                for (auto* td : unit->typeDeclaration()) {
                    if (auto* cd = td->classDeclaration()) { classDecl = cd; break; }
                }
                if (!classDecl) {
                    throw Exception(
                        "companion synthesizer '" + label
                            + "' emitted source with no class declaration",
                        "CAJETA_ERROR_SYNTH_FAILED");
                }
                auto qName = QualifiedName::getOrInsert(
                    res.className, res.packageName);
                std::list<QualifiedNamePtr> qExt;
                std::list<QualifiedNamePtr> qImpl;
                {
                    auto kwIdx = [](antlr4::tree::TerminalNode* n) -> ssize_t {
                        return n && n->getSymbol()
                            ? (ssize_t) n->getSymbol()->getTokenIndex() : -1;
                    };
                    ssize_t extKw = kwIdx(classDecl->EXTENDS());
                    ssize_t implKw = kwIdx(classDecl->IMPLEMENTS());
                    for (auto* tl : classDecl->typeList()) {
                        ssize_t tlIdx = tl->getStart()
                            ? (ssize_t) tl->getStart()->getTokenIndex() : -1;
                        ssize_t best = -1;
                        int which = -1;
                        if (extKw >= 0 && extKw < tlIdx && extKw > best) {
                            best = extKw;
                            which = 0;
                        }
                        if (implKw >= 0 && implKw < tlIdx && implKw > best) {
                            best = implKw;
                            which = 1;
                        }
                        std::list<QualifiedNamePtr>* bucket =
                            which == 0 ? &qExt : which == 1 ? &qImpl : nullptr;
                        if (!bucket) continue;
                        for (auto& tt : tl->typeType()) {
                            if (auto* coi = tt->classOrInterfaceType()) {
                                bucket->push_back(QualifiedName::fromContext(coi));
                            }
                        }
                    }
                }
                CajetaClassPtr klass;
                if (existing) {
                    existing->fillFromDeclaration(pModule, qName, qExt, qImpl);
                    klass = existing;
                } else {
                    klass = std::make_shared<CajetaClass>(
                        pModule, qName, qExt, qImpl);
                }
                // Register BEFORE the body walk so self-references and the
                // triggering unit's later name lookups resolve.
                cmap[canonical] = klass;
                cmap[res.className] = klass;
                pModule->getStructures()[canonical] = klass;
                // Registration ONLY: this hook has no live builder, so the codegen
                // fixed-point loop emits the companion's prototypes and bodies.
                auto& stk = pModule->getStructureStack();
                std::list<CajetaClassPtr> savedStack;
                savedStack.swap(stk);
                stk.push_back(klass);
                try {
                    auto bodyAny = visitClassBody(classDecl->classBody());
                    auto classBody =
                        std::any_cast<ClassBodyDeclarationPtr>(bodyAny);
                    for (auto& decl : classBody->getDeclarations()) {
                        decl->updateParent(klass);
                    }
                    klass->generatePrototype();
                } catch (...) {
                    stk.clear();
                    stk.swap(savedStack);
                    throw;
                }
                stk.clear();
                stk.swap(savedStack);
            }
        }

        // Run every registered member synthesizer that claims `structure`, parse each
        // `{ ... }` fragment, and reparent its members onto the target. A member that
        // collides with an existing one is an error, not last-writer-wins.
        void runMemberSynthesizers(CajetaClassPtr structure) {
            if (!structure) return;
            cajeta::synth::registerBuiltinSynthesizers();
            cajeta::synth::SynthesisContext ctx;
            ctx.parent = structure;
            ctx.module = pModule;
            auto claimed = cajeta::synth::SynthesizerRegistry::instance()
                .collectMembers(ctx);
            if (claimed.empty()) return;
            std::set<std::string> seenFields;
            std::set<std::string> seenMethods;
            auto methodKey = [](const MethodPtr& m) {
                std::string key = m->getName() + "(";
                bool first = true;
                for (auto& p : m->getParameterList()) {
                    if (!first) key += ",";
                    first = false;
                    key += (p && p->getType())
                        ? p->getType()->toCanonical() : std::string("?");
                }
                return key + ")";
            };
            for (auto& p : structure->getPropertyList()) {
                if (p) seenFields.insert(p->getName());
            }
            for (auto& kv : structure->getMethods()) {
                if (kv.second) seenMethods.insert(methodKey(kv.second));
            }
            auto collide = [&](const string& label, const string& memberName) {
                throw Exception(
                    "source-synthesis member collision: synthesizer '"
                        + label + "' injects member '" + memberName
                        + "' which already exists on "
                        + structure->getQName()->toCanonical()
                        + " — no last-writer-wins",
                    "CAJETA_ERROR_SYNTH_MEMBER_COLLISION");
            };
            for (auto& [label, res] : claimed) {
                for (auto& imp : res.imports) {
                    // Prescan the package BEFORE binding the name, or an import from
                    // a lazy package binds with no archive entry behind it.
                    if (CajetaModule::stdlibImportHook) {
                        CajetaModule::stdlibImportHook(imp.second);
                    }
                    cajeta::synth::injectImportIfUnbound(pModule, imp.first, imp.second);
                }
                // Synthesized members have no source file; the mask keeps their
                // callees from being attributed to a real call site.
                xref::SyntheticSourceScope xrefMask;
                auto* body = cajeta::synth::parseClassBodyFragment(res.classBodyFragment);
                for (auto* cbd : body->classBodyDeclaration()) {
                    MemberDeclarationPtr mem;
                    try {
                        mem = std::any_cast<MemberDeclarationPtr>(
                            visitClassBodyDeclaration(cbd));
                    } catch (ReuseHazardAbort&) {
                        throw;  // reuse rollback must reach the compile driver
                    } catch (...) { continue; }
                    if (auto fieldDecl =
                            std::dynamic_pointer_cast<FieldDeclaration>(mem)) {
                        std::size_t before = structure->getPropertyList().size();
                        fieldDecl->updateParent(structure);
                        auto& plist = structure->getPropertyList();
                        auto it = plist.begin();
                        std::advance(it, before);
                        for (; it != plist.end(); ++it) {
                            if (*it && !seenFields.insert((*it)->getName()).second) {
                                collide(label, (*it)->getName());
                            }
                        }
                        continue;
                    }
                    if (auto methodDecl =
                            std::dynamic_pointer_cast<MethodDeclaration>(mem)) {
                        const string memberName = methodDecl->getMethod()
                            ? methodDecl->getMethod()->getName() : string();
                        // Constructors OVERLOAD by design and are exempt; a truly
                        // duplicate signature still fails in method registration.
                        const bool isCtor = methodDecl->getMethod()
                            && methodDecl->getMethod()->isConstructor();
                        if (!memberName.empty() && !isCtor
                                && !seenMethods.insert(
                                        methodKey(methodDecl->getMethod()))
                                    .second) {
                            collide(label, memberName);
                        }
                        if (methodDecl->getMethod()) {
                            methodDecl->getMethod()->setSynthesizedMember(true);
                        }
                        methodDecl->updateParent(structure);
                        continue;
                    }
                }
            }
        }

        // Parents of a class-like during the visit pass: the resolved superClasses
        // when present, else the declared `extends` names via canonicalMap
        // (resolveSuperClasses has not run yet at synthesis time).
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

        // Append `pathA.f == pathB.f` terms for every instance field of `cls` onto
        // `expr`, ancestors first. Inline arrays expand per element and nested
        // value-type fields flatten to primitive leaves.
        void appendRecordFieldCompares(const string& pathA, const string& pathB,
                const CajetaClassPtr& cls, string& expr) {
            for (auto& sup : resolvedParentsOf(cls)) {
                appendRecordFieldCompares(pathA, pathB, sup, expr);
            }
            for (auto& prop : cls->getPropertyList()) {
                if (!prop || prop->isStatic()) continue;
                auto ft = prop->getType();
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

        // Synthesize a static field-wise `operator==` for a record that declares none;
        // `!=` derives from it. Fields flatten to primitive-leaf compares because a
        // by-value operator argument currently marshals as a pointer.
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
            xref::SyntheticSourceScope xrefMask;
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

        // Flatten a record's instance fields (ancestors first, declared order) into
        // (a.path, b.path) pairs. Clears `orderable` when a field has no natural `<`
        // — only primitive fields qualify.
        void collectRecordFieldPaths(const string& pathA, const string& pathB,
                const CajetaClassPtr& cls,
                std::vector<std::pair<string, string>>& out, bool& orderable) {
            for (auto& sup : resolvedParentsOf(cls)) {
                collectRecordFieldPaths(pathA, pathB, sup, out, orderable);
            }
            for (auto& prop : cls->getPropertyList()) {
                if (!prop || prop->isStatic()) continue;
                auto ft = prop->getType();
                // Only PRIMITIVE fields are orderable: a value-type field needs its
                // OWN `operator<`, which only records get synthesized, so `a.f < b.f`
                // on a `@ValueType` like Utf8 would crash.
                bool isPrim = ft && (ft->getTypeFlags() & PRIMITIVE_FLAG) != 0;
                if (!isPrim) orderable = false;
                out.emplace_back(pathA + "." + prop->getName(),
                                 pathB + "." + prop->getName());
            }
        }

        // Synthesize a lexicographic `operator<` for a record that declares none and
        // whose fields are all orderable, so it can be sorted. `>`, `<=` and `>=`
        // derive from it via OperatorDispatch.
        void synthesizeRecordOrdering(CajetaClassPtr structure) {
            for (auto& kv : structure->getMethods()) {
                if (kv.second && kv.second->getName() == "operator<") return;
            }
            std::vector<std::pair<string, string>> fields;
            bool orderable = true;
            collectRecordFieldPaths("a", "b", structure, fields, orderable);
            if (!orderable || fields.empty()) return;
            string expr, eqPrefix;
            for (auto& f : fields) {
                string lessF = f.first + " < " + f.second;
                string eqF = f.first + " == " + f.second;
                string term = eqPrefix.empty()
                    ? lessF : ("(" + eqPrefix + " && " + lessF + ")");
                expr = expr.empty() ? term : (expr + " || " + term);
                eqPrefix = eqPrefix.empty() ? eqF : (eqPrefix + " && " + eqF);
            }
            string typeName = structure->getQName()->getTypeName();
            string src = "{ public static boolean operator< (" + typeName
                + " a, " + typeName + " b) { return " + expr + "; } }";
            xref::SyntheticSourceScope xrefMask;
            auto* body = cajeta::synth::parseClassBodyFragment(src);
            for (auto* cbd : body->classBodyDeclaration()) {
                MemberDeclarationPtr mem;
                try {
                    mem = std::any_cast<MemberDeclarationPtr>(
                        visitClassBodyDeclaration(cbd));
                } catch (ReuseHazardAbort&) {
                    throw;
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
            for (auto& scriptMemberContext: ctx->scriptMember()) {
                if (scriptMemberContext->typeDeclaration() != nullptr) {
                    pModule->onStructureDeclaration(
                        visitChildren(scriptMemberContext->typeDeclaration()));
                }
            }
            return std::any(nullptr);
        }

        virtual std::any visitScriptMember(CajetaParser::ScriptMemberContext* ctx) override {
            return visitChildren(ctx);
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

        // Record a declaration's NAME-token position on the built type for the xref
        // export — the name token, not the declaration start, because that is what an
        // IDE navigates to and renames.
        static void captureDeclPosition(const CajetaClassPtr& built,
                                        antlr4::ParserRuleContext* nameCtx) {
            if (!built || !nameCtx || !nameCtx->getStart()) return;
            built->setDeclPosition(
                (int) nameCtx->getStart()->getLine(),
                (int) nameCtx->getStart()->getCharPositionInLine());
        }

        // Capture what an IDE needs to navigate to a template's members: name,
        // position, and parameter types AS WRITTEN. A template's body walk is skipped,
        // so it holds no Method objects; this deliberately resolves nothing.
        void captureTemplateMembers(antlr4::ParserRuleContext* ctx,
                                    const CajetaClassPtr& structure) {
            if (!ctx || !structure) return;
            const string ownerFqn = structure->getQName()->toCanonical();
            const string file     = pModule->currentSourceFile();

            CajetaParser::ClassBodyContext* body = nullptr;
            for (auto* child : ctx->children) {
                if ((body = dynamic_cast<CajetaParser::ClassBodyContext*>(child))) break;
            }
            if (!body) return;

            // Parameter types AS WRITTEN — `(T)`, `(int32, T)`, `()`.
            auto paramText = [](CajetaParser::FormalParametersContext* fp) -> string {
                string out = "(";
                if (fp && fp->formalParameterList()) {
                    bool first = true;
                    for (auto* p : fp->formalParameterList()->formalParameter()) {
                        if (!p->typeType()) continue;
                        if (!first) out += ",";
                        out += p->typeType()->getText();
                        first = false;
                    }
                }
                return out + ")";
            };

            // Declared parameter count — excludes the receiver, which the compiler's
            // own canonical form includes.
            auto paramCount = [](CajetaParser::FormalParametersContext* fp) -> int {
                if (!fp || !fp->formalParameterList()) return 0;
                return (int) fp->formalParameterList()->formalParameter().size();
            };

            // The DISPLAY label — `T get(int32 index)`, as written. Distinct from the
            // overload key, which is the identity.
            auto displayText = [](const string& ret, const string& name,
                                  CajetaParser::FormalParametersContext* fp) -> string {
                string out;
                if (!ret.empty()) out += ret + " ";
                out += name + "(";
                if (fp && fp->formalParameterList()) {
                    bool first = true;
                    for (auto* p : fp->formalParameterList()->formalParameter()) {
                        if (!p->typeType()) continue;
                        if (!first) out += ", ";
                        first = false;
                        if (p->REFERENCE()) out += "#";
                        out += p->typeType()->getText();
                        if (p->variableDeclaratorId()) {
                            out += " " + p->variableDeclaratorId()->getText();
                        }
                    }
                }
                return out + ")";
            };

            bool memberIsStatic = false;   // set per classBodyDeclaration below

            auto note = [&](const string& name, const string& kind,
                            const string& key, antlr4::Token* tok,
                            int declaredParams = 0,
                            const string& signature = "") {
                if (!tok) return;
                xref::TemplateMember m;
                m.ownerFqn       = ownerFqn;
                m.name           = name;
                m.kind           = kind;
                m.overloadKey    = key;
                m.signature      = signature;
                m.declaredParams = declaredParams;
                m.isStatic       = memberIsStatic;
                m.at             = xref::SourceRef{file, (int) tok->getLine(),
                                                   (int) tok->getCharPositionInLine()};
                xref::registerTemplateMember(std::move(m));
            };

            for (auto* decl : body->classBodyDeclaration()) {
                auto* member = decl->memberDeclaration();
                if (!member) continue;

                // A static member has no receiver, so its expected instantiation
                // arity is its declared arity, not declared+1.
                memberIsStatic = false;
                for (auto* mod : decl->modifier()) {
                    if (mod->getText() == "static") { memberIsStatic = true; break; }
                }

                if (auto* md = member->methodDeclaration()) {
                    const string name = md->identifier()->getText();
                    const string ret  = md->typeTypeOrVoid()
                                      ? md->typeTypeOrVoid()->getText() : string("void");
                    note(name, "method",
                         ownerFqn + "::" + name + paramText(md->formalParameters()),
                         md->identifier()->getStart(),
                         paramCount(md->formalParameters()),
                         displayText(ret, name, md->formalParameters()));
                } else if (auto* cd = member->constructorDeclaration()) {
                    const string name = cd->identifier()->getText();
                    note(name, "constructor",
                         ownerFqn + "::" + name + paramText(cd->formalParameters()),
                         cd->identifier()->getStart(),
                         paramCount(cd->formalParameters()),
                         displayText("", name, cd->formalParameters()));
                } else if (auto* od = member->operatorOverloadDeclaration()) {
                    // Recover the actual symbol (`operator+`, `operator[]=`) from the
                    // tokens between OPERATOR and the parameter list: a bare
                    // "operator" would name every operator identically.
                    string sym;
                    bool afterKw = false;
                    for (auto* child : od->children) {
                        if (dynamic_cast<CajetaParser::FormalParametersContext*>(child)) break;
                        if (!afterKw) {
                            auto* term = dynamic_cast<antlr4::tree::TerminalNode*>(child);
                            if (term && term == od->OPERATOR()) afterKw = true;
                            continue;
                        }
                        sym += child->getText();
                    }
                    const string name = "operator" + sym;
                    const string ret  = od->typeTypeOrVoid()
                                      ? od->typeTypeOrVoid()->getText() : string("void");
                    note(name, "method",
                         ownerFqn + "::" + name + paramText(od->formalParameters()),
                         od->OPERATOR() ? od->OPERATOR()->getSymbol() : nullptr,
                         paramCount(od->formalParameters()),
                         displayText(ret, name, od->formalParameters()));
                } else if (auto* fd = member->fieldDeclaration()) {
                    if (!fd->variableDeclarators()) continue;
                    const string ftype = fd->typeType() ? fd->typeType()->getText() : string();
                    for (auto* vd : fd->variableDeclarators()->variableDeclarator()) {
                        if (!vd->variableDeclaratorId()) continue;
                        const string name = vd->variableDeclaratorId()->getText();
                        note(name, "field", "", vd->variableDeclaratorId()->getStart(),
                             0, ftype.empty() ? name : ftype + " " + name);
                    }
                }
            }
        }

        virtual std::any visitClassDeclaration(CajetaParser::ClassDeclarationContext* ctx) override {
            auto built = buildClassLike(ctx, ctx->identifier()->getText(),
                ctx->typeParameters(), ctx->EXTENDS(), ctx->IMPLEMENTS(),
                ctx->PERMITS(), ctx->typeList(), /*isRecord=*/false);
            captureDeclPosition(built, ctx->identifier());
            return std::any(built);
        }

        virtual std::any visitRecordDeclaration(CajetaParser::RecordDeclarationContext* ctx) override {
            auto built = buildClassLike(ctx, ctx->identifier()->getText(),
                ctx->typeParameters(), ctx->EXTENDS(), ctx->IMPLEMENTS(),
                nullptr, ctx->typeList(), /*isRecord=*/true);
            captureDeclPosition(built, ctx->identifier());
            return std::any(built);
        }

        // Shared builder for class-like declarations (class / record): resolves the
        // qualified name, buckets extends/implements/permits by keyword position,
        // reuses any placeholder, walks the body, and generates the prototype.
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
            if (getenv("CAJETA_DBG_MATERIALIZE")) {
                std::cerr << "[walk] buildClassLike " << qName->toCanonical()
                          << " (module=" << pModule->getSourcePath() << ")\n";
            }
            list<QualifiedNamePtr> qExtended;
            list<QualifiedNamePtr> qImplemented;
            // Type arguments per implements entry, parallel to qImplemented (empty
            // for a non-templated interface reference).
            list<vector<QualifiedNamePtr>> qImplementedTypeArgs;
            // Grammar: `(EXTENDS typeList)? (IMPLEMENTS typeList)? (PERMITS typeList)?`
            // — ANTLR exposes typeLists in source order, so match each to its keyword
            // by start-token index. PERMITS parses but is ignored in v1.
            auto kwIdx = [](antlr4::tree::TerminalNode* n) -> ssize_t {
                return n && n->getSymbol() ? (ssize_t) n->getSymbol()->getTokenIndex() : -1;
            };
            ssize_t extKw = kwIdx(extendsKw);
            ssize_t implKw = kwIdx(implementsKw);
            ssize_t permKw = kwIdx(permitsKw);
            for (auto* tl : typeLists) {
                ssize_t tlIdx = tl->getStart()
                    ? (ssize_t) tl->getStart()->getTokenIndex() : -1;
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
                    // Type args come off the LEAF identifier; multi-level qualified
                    // templates (`Outer<A>.Inner<B>`) are not supported in v1.
                    if (which == 1) {
                        vector<QualifiedNamePtr> args;
                        // typeArguments has one slot per identifier in the dotted
                        // chain; the leaf's is the last non-null entry.
                        auto targsList = coi->typeArguments();
                        CajetaParser::TypeArgumentsContext* leafTargs = nullptr;
                        for (auto* ta : targsList) {
                            if (ta) leafTargs = ta;
                        }
                        if (leafTargs) {
                            for (auto* targ : leafTargs->typeArgument()) {
                                if (!targ || !targ->typeType()) continue;
                                if (auto* targCoi = targ->typeType()
                                        ->classOrInterfaceType()) {
                                    args.push_back(
                                        QualifiedName::fromContext(targCoi));
                                } else if (auto* targPrim = targ->typeType()
                                        ->primitiveType()) {
                                    // Primitives live in CAJETA_NATIVE_PACKAGE (""),
                                    // and canonMap finds them by short name.
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
            // Auto-extend Object: a class with no explicit `extends` inherits
            // cajeta.lang.Object. Object itself is skipped (self-cycle); interfaces
            // and enums take separate visitor paths.
            bool isObjectItself =
                qName->getTypeName() == "Object" &&
                qName->getPackageName() == "cajeta.lang";
            if (qExtended.empty() && !isObjectItself) {
                qExtended.push_back(
                    QualifiedName::getOrInsert("Object", "cajeta.lang"));
            }
            // Placeholder reuse: an earlier forward reference may already hold this
            // class's canonical (or short) name. Fill THAT shared_ptr so every
            // earlier reference now points at the fully-filled class.
            CajetaClassPtr structure;
            {
                auto& canon = CajetaType::getCanonicalMap();
                auto it = canon.find(qName->toCanonical());
                if (it == canon.end()) {
                    // Short-name fallback, guarded: the short key can hold a
                    // placeholder made for ANOTHER package's same-named class, so
                    // accept only a hit whose recorded canonical is ours.
                    auto sit = canon.find(qName->getTypeName());
                    if (sit != canon.end()) {
                        auto ph = std::dynamic_pointer_cast<CajetaClass>(
                            sit->second);
                        if (ph && ph->getQName()
                                && ph->getQName()->toCanonical()
                                       == qName->toCanonical()) {
                            it = sit;
                        }
                    }
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

            // A record is a final @ValueType with no vtable; synthesizing the
            // annotation routes it through every existing @ValueType path.
            if (isRecord) {
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

            // Template parameters: name plus optional `extends` bounds. The class
            // becomes a template, and the enclosing declaration's raw source is
            // captured so the parse tree can be released when this pass ends.
            if (auto* tps = typeParametersCtx) {
                vector<TypeParameter> params;
                for (auto* tp : tps->typeParameter()) {
                    TypeParameter param(tp->identifier()->getText());
                    if (tp->REFERENCE() != nullptr) {
                        throw Exception(
                            "`#` on a type parameter declaration is retired: "
                            "ownership is per-call under title-tracking "
                            "(specs/title-tracking-spec.md §8.1) — drop the "
                            "`#` (a must-own edge is spelled on the FORMAL)",
                            "CAJETA_ERROR_TYPE_TRANSFER_RETIRED");
                    }
                    param.owningRequired = false;
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

            // Annotations and keyword modifiers sit on the TypeDeclaration for a
            // top-level class but on the enclosing classBodyDeclaration for a NESTED
            // one; gather from whichever applies.
            std::vector<CajetaParser::ClassOrInterfaceModifierContext*> coims;
            if (auto* typeDecl = dynamic_cast<CajetaParser::TypeDeclarationContext*>(ctx->parent)) {
                for (auto* m : typeDecl->classOrInterfaceModifier()) coims.push_back(m);
            } else if (auto* memberDecl = dynamic_cast<CajetaParser::MemberDeclarationContext*>(ctx->parent)) {
                if (auto* cbd = dynamic_cast<CajetaParser::ClassBodyDeclarationContext*>(memberDecl->parent)) {
                    for (auto* m : cbd->modifier()) {
                        if (auto* coim = m->classOrInterfaceModifier()) coims.push_back(coim);
                    }
                }
            }
            {
                for (auto* mod : coims) {
                    // A classOrInterfaceModifier is EITHER an annotation OR a
                    // keyword: annotation() is null for the keyword form.
                    if (!mod->annotation()) {
                        // `abstract` maps to Modifier::NONE, so gate on the raw
                        // keyword text.
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
                        // Cache @SuppressLint's rule IDs so isLintSuppressed walks a
                        // tiny vector instead of the annotation list per call.
                        if (inst->getName()->getTypeName() == "SuppressLint") {
                            for (auto& id : inst->getStringList()) {
                                structure->addSuppressedLint(id);
                            }
                            const string& single = inst->getString();
                            if (!single.empty()) structure->addSuppressedLint(single);
                        }
                    }
                }
            }

            // `@Sealed` bars reflective access to private members; recorded as a class
            // modifier so it rides into the RTTI header's modifiers word.
            if (structure->findAnnotation("Sealed")) {
                structure->addModifier(REFLECT_SEALED);
            }

            // `@Retained` keeps a class in the Class.forName registry. Advisory until
            // AOT stripping lands; recorded as a modifier for the RTTI word.
            if (structure->findAnnotation("Retained")) {
                structure->addModifier(REFLECT_RETAINED);
            }

            pModule->getStructureStack().push_back(structure);
            // Mid-walk marker (CajetaClass::declWalkInFlight): a nested materialize
            // compile's prototype sweep must leave this class pending until the walk
            // below completes.
            structure->setDeclWalkInFlight(true);
            // An `@Aspect` class joins the process-global aspect registry the
            // pointcut-matching pass walks at codegen time. A template registers when
            // an instantiation re-runs this visit.
            if (structure->findAnnotation("Aspect")) {
                CajetaModule::registerAspectClass(structure);
            }
            // @Component / @Repository / @TestComponent all register as DI
            // participants (the last flips isTestComponent). Every @Profile
            // occurrence contributes; an empty list means profile-neutral.
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
                            // The array form yields a StringList, the single form a
                            // String; repeated annotations accumulate.
                            const vector<string>& list = inst->getStringList();
                            if (!list.empty()) {
                                for (auto& p : list) {
                                    if (!p.empty()) desc->profiles.push_back(p);
                                }
                            } else {
                                const string& p = inst->getString();
                                if (!p.empty()) desc->profiles.push_back(p);
                            }
                        }
                    }
                    CajetaModule::registerComponent(desc);
                }
            }
            // Pre-register under both canonical and short name so self-references
            // inside the body resolve, and so a later reference to a class whose
            // prototype is deferred cannot create a second, never-filled placeholder.
            CajetaType::getCanonicalMap()[qName->toCanonical()] =
                static_pointer_cast<CajetaType>(structure);
            CajetaType::getCanonicalMap()[qName->getTypeName()] =
                static_pointer_cast<CajetaType>(structure);

            // A template's body walk is skipped: it references unresolved type
            // parameters. The captured snippet is the body's source of truth, re-parsed
            // per instantiation — so capture members for xref BEFORE the skip.
            if (structure->isTemplate() && xref::captureEnabled()) {
                captureTemplateMembers(ctx, structure);
            }
            if (!structure->isTemplate()) {
                structure->setClassBody(std::any_cast<ClassBodyDeclarationPtr>(visitChildren(ctx)));

                // Hash / equals + != / == consistency checks. Templates skip them
                // here and re-check at instantiation.
                bool hasOpEq = false;
                bool hasOpNe = false;
                bool hasHash = false;
                // The methods map is keyed by canonical signature, so match on the
                // method's bare name rather than the key.
                for (auto& kv : structure->getMethods()) {
                    const std::string& mname = kv.second->getName();
                    if (mname == "operator==") hasOpEq = true;
                    if (mname == "operator!=") hasOpNe = true;
                    if (mname == "hash")       hasHash = true;
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
                // @AutoHash (and @Data / @Value, which imply it) synthesize hash()
                // later in tryGeneratePrototype, so the annotation satisfies it here.
                if (structure->findAnnotation("AutoHash")
                        || structure->findAnnotation("Data")
                        || structure->findAnnotation("Value")) {
                    hasHash = true;
                }
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
                // @Factory discovery runs here because it needs the provider methods
                // populated. A @Factory is ALSO registered as a plain @Component so
                // its collaborators resolve and it is itself injectable.
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
                        // A provider returns the product; void methods are helpers.
                        if (!retType || retType->toCanonical() == "void") continue;
                        CajetaModule::FactoryProvider prov;
                        prov.method = method;
                        prov.providedType = retType;
                        // R4: method scope @Singleton / @Transient (Singleton default).
                        prov.scope = method->findAnnotation("Transient")
                            ? CajetaModule::AllocateMode::Transient
                            : CajetaModule::AllocateMode::Singleton;
                        // Classify each param: @Inject is an edge, unmarked is
                        // assisted (any assisted means factory-injection).
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
            // Member synthesizers fire BEFORE the prototype is built, so injected
            // members are laid out and resolve inside the class's own method bodies.
            runMemberSynthesizers(structure);
            // A template record's body walk is skipped, so it has no fields yet.
            if (isRecord && !structure->isTemplate()) {
                synthesizeRecordEquality(structure);
                synthesizeRecordOrdering(structure);
            }
            // tryGeneratePrototype is the deferred-aware variant: with a placeholder
            // parent it returns false untouched, and buildPendingPrototypes walks
            // canonicalMap to fixed-point once every module's parse completes.
            structure->tryGeneratePrototype();
            // @ValueType marks a by-value POD class eligible for operator-overload
            // dispatch. Runs AFTER tryGeneratePrototype so fields are populated; the
            // POD check mirrors isPodStruct in KernelArgTrait.
            if (structure->findAnnotation("ValueType")) {
                // A generic @ValueType's field types are still placeholders, so
                // POD-ness validates at each concrete instantiation instead.
                if (!structure->isTemplate()) {
                    const string kind = isRecord ? "record" : "@ValueType class";
                    if (isRecord) {
                        // Every ancestor must itself be a record; Object, the
                        // fieldless auto-extend root, is exempt.
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
                        // A derived record may not shadow an inherited INSTANCE
                        // method; statics and constructors dispatch statically.
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
                                            // A SYNTHESIZED parent member (the auto
                                            // value-clone) may be shadowed: static
                                            // dispatch picks by declared type.
                                            if (sm->isSynthesizedMember()) continue;
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
                // VALUE_TYPE_FLAG is the kind (it relaxes the operator-dispatch
                // !PRIMITIVE_FLAG gate); BY_VALUE_FLAG is the storage axis: inline
                // slot, Copy, no drop or borrow.
                structure->addTypeFlags(VALUE_TYPE_FLAG | BY_VALUE_FLAG);
            }
            pModule->getStructureStack().pop_back();
            structure->setDeclWalkInFlight(false);
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

        // Build a CajetaView — a typed zero-copy overlay onto a byte buffer — from a
        // `view` declaration, reading endianness and alignment annotations off the
        // enclosing typeDeclaration.
        virtual std::any visitViewDeclaration(CajetaParser::ViewDeclarationContext* ctx) override {
            const string& name = ctx->identifier()->getText();
            string packageAdj;
            for (auto& structure : pModule->getStructureStack()) {
                packageAdj.append(".");
                packageAdj.append(structure->getQName()->getTypeName());
            }
            QualifiedNamePtr qName = QualifiedName::getOrInsert(
                name, pModule->getQName()->getPackageName() + packageAdj);
            // Placeholder reuse, as in visitClassDeclaration: fill the SAME shared_ptr
            // so every captured forward reference becomes the real view.
            shared_ptr<CajetaView> viewStructure;
            {
                auto& canon = CajetaType::getCanonicalMap();
                auto it = canon.find(qName->toCanonical());
                if (it == canon.end()) {
                    // Short-name fallback, guarded against cross-package capture.
                    auto sit = canon.find(qName->getTypeName());
                    if (sit != canon.end()) {
                        auto ph = dynamic_pointer_cast<CajetaClass>(
                            sit->second);
                        if (ph && ph->getQName()
                                && ph->getQName()->toCanonical()
                                       == qName->toCanonical()) {
                            it = sit;
                        }
                    }
                }
                if (it != canon.end()) {
                    auto existing = dynamic_pointer_cast<CajetaView>(it->second);
                    if (existing && existing->isPlaceholder()) {
                        existing->fillFromDeclaration(
                            pModule, qName, {}, {});
                        viewStructure = existing;
                    }
                }
            }
            if (!viewStructure) {
                viewStructure = make_shared<CajetaView>(pModule, qName);
            }
            CajetaClassPtr structure = static_pointer_cast<CajetaClass>(viewStructure);
            captureDeclPosition(structure, ctx->identifier());

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

        // Register an enum as an i32-backed type: each constant gets an ordinal, and
        // an enum BODY's members register on a "$enum" companion class whose instance
        // methods take the ordinal as `this`. No object, no vtable, no subclassing.
        virtual std::any visitEnumDeclaration(CajetaParser::EnumDeclarationContext* ctx) override {
            string name = ctx->identifier()->getText();
            string packageAdj;
            for (auto& s : pModule->getStructureStack()) {
                packageAdj.append(".");
                packageAdj.append(s->getQName()->getTypeName());
            }
            QualifiedNamePtr qName = QualifiedName::getOrInsert(
                name, pModule->getQName()->getPackageName() + packageAdj);

            // shareLlvmType=false: i32 already owns the typeMap[i32] slot and must
            // not be clobbered.
            llvm::Type* i32Ty = llvm::Type::getInt32Ty(*pModule->getLlvmContext());
            auto enumType = CajetaType::create(qName, i32Ty,
                INT_FLAG | SIGNED_FLAG | NUMBER_FLAG | PRIMITIVE_FLAG
                    | BIT_32_FLAG | ENUM_FLAG,
                /*shareLlvmType=*/false);
            // Also register the short name so an unqualified `Color` resolves.
            CajetaType::getCanonicalMap()[qName->getTypeName()] = enumType;

            // An enum is a CajetaType, not a CajetaClass, so its declaring file and
            // position must be stamped here or the xref export cannot locate it.
            enumType->setDeclaringFile(pModule->currentSourceFile());
            if (ctx->identifier() && ctx->identifier()->getStart()) {
                enumType->setDeclPosition(
                    (int) ctx->identifier()->getStart()->getLine(),
                    (int) ctx->identifier()->getStart()->getCharPositionInLine());
            }

            int32_t ordinal = 0;
            if (auto* constants = ctx->enumConstants()) {
                for (auto* ec : constants->enumConstant()) {
                    string constName = ec->identifier()->getText();
                    CajetaType::registerEnumConstant(name, constName, ordinal++);
                    // The ordinal registry carries no position, so record each
                    // constant's own or Ctrl-click on `GREEN` lands on `Color`.
                    if (ec->identifier()->getStart()) {
                        CajetaType::registerEnumConstantPosition(
                            name, constName,
                            pModule->currentSourceFile(),
                            (int) ec->identifier()->getStart()->getLine(),
                            (int) ec->identifier()->getStart()->getCharPositionInLine());
                    }
                }
            }

            // --- enum body: members live on a companion class ---------------
            // Reuses the class-body member-registration path; the "$enum" key cannot
            // collide with the i32 enum type in the enum's own canonical slot.
            if (auto* body = ctx->enumBodyDeclarations()) {
                QualifiedNamePtr cName = QualifiedName::getOrInsert(
                    name + "$enum",
                    pModule->getQName()->getPackageName() + packageAdj);
                list<QualifiedNamePtr> noExtends;
                list<QualifiedNamePtr> noImplements;
                auto companion = make_shared<CajetaClass>(
                    pModule, cName, noExtends, noImplements);
                // Enums are implicitly final, which licenses the static dispatch.
                companion->addModifier(FINAL);

                pModule->getStructureStack().push_back(companion);
                // Mirror visitClassBody: a bare visitChildren walks the members but
                // registers nothing.
                ClassBodyDeclarationPtr classBody =
                    make_shared<ClassBodyDeclaration>(body->getStart());
                for (auto* cbd : body->classBodyDeclaration()) {
                    classBody->getDeclarations().push_back(
                        std::any_cast<MemberDeclarationPtr>(
                            visitClassBodyDeclaration(cbd)));
                }
                companion->setClassBody(classBody);

                // Give every instance method an explicit `this` typed as the ENUM
                // (i32) before prototypes are generated: Method's own injection would
                // splice in a pointer receiver, meaningless for an ordinal.
                for (auto& m : companion->getMethodList()) {
                    if (!m || m->isStatic()) continue;
                    m->prependThisParameter(enumType);
                }

                companion->generatePrototype();
                pModule->getStructureStack().pop_back();
                CajetaType::getCanonicalMap()[cName->toCanonical()] = companion;
                CajetaType::getCanonicalMap()[cName->getTypeName()] = companion;
                // Register with the MODULE too: getAllMethods() walks `structures`,
                // and that is what the codegen driver lowers.
                pModule->getStructures()[cName->toCanonical()] = companion;
                CajetaModule::getStructureToModule()[cName->toCanonical()] = pModule;
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

        // Function-type production `(T1, T2) -> R`. The CajetaFunctionType is built by
        // CajetaType::fromContext on the enclosing typeType; this hook only descends.
        virtual std::any visitFunctionType(CajetaParser::FunctionTypeContext* ctx) override {
            return visitChildren(ctx);
        }

        // Build a CajetaClass tagged isInterface: the body holds abstract method
        // signatures, and each implementing class's vtable gets one slot per interface
        // method. No default methods, nested types, or method-level generics in v1.
        virtual std::any visitInterfaceDeclaration(CajetaParser::InterfaceDeclarationContext* ctx) override {
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

            // Placeholder reuse, as in visitClassDeclaration, but NON-GENERIC only:
            // that is exactly the set fromContext gives a fat placeholder, and reusing
            // a generic one mis-seeds the template's instantiation.
            bool isGenericIface = ctx->typeParameters() != nullptr;
            CajetaClassPtr interface;
            if (!isGenericIface) {
                auto& canon = CajetaType::getCanonicalMap();
                auto it = canon.find(qName->toCanonical());
                if (it == canon.end()) {
                    // Short-name fallback, guarded against cross-package capture.
                    auto sit = canon.find(qName->getTypeName());
                    if (sit != canon.end()) {
                        auto ph = std::dynamic_pointer_cast<CajetaClass>(
                            sit->second);
                        if (ph && ph->getQName()
                                && ph->getQName()->toCanonical()
                                       == qName->toCanonical()) {
                            it = sit;
                        }
                    }
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
            captureDeclPosition(interface, ctx->identifier());
            // The interface path never attaches annotations to the structure, so read
            // the enclosing typeDeclaration's modifiers directly.
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

            // Templated interfaces mirror the class-template handling: capture the
            // type parameters so isTemplate() holds, and the body walk below skips
            // method emission (the body references unresolved T placeholders).
            if (auto* tps = ctx->typeParameters()) {
                vector<TypeParameter> params;
                for (auto* tp : tps->typeParameter()) {
                    TypeParameter param(tp->identifier()->getText());
                    if (tp->REFERENCE() != nullptr) {
                        throw Exception(
                            "`#` on a type parameter declaration is retired: "
                            "ownership is per-call under title-tracking "
                            "(specs/title-tracking-spec.md §8.1) — drop the "
                            "`#` (a must-own edge is spelled on the FORMAL)",
                            "CAJETA_ERROR_TYPE_TRANSFER_RETIRED");
                    }
                    param.owningRequired = false;
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

                // Capture the full declaration source so CajetaClass::instantiate can
                // re-parse the body under a type-parameter substitution.
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

            // Build abstract Methods directly rather than through visitClassBody /
            // visitMethodDeclaration, which expect a real body. A templated interface
            // skips emission for the same reason a class template does.
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
                    // `#T foo();` — an interface method's return transfers ownership.
                    // This path builds its Method by hand, so it must read the `#` off
                    // typeTypeOrVoid itself rather than inheriting the class-body path.
                    if (common->typeTypeOrVoid()
                            && common->typeTypeOrVoid()->REFERENCE() != nullptr) {
                        method->setReturnsOwnership(true);
                    }
                    // `^T` — the VIEW stance. An implementor's body is invisible at
                    // the call site, so the signature is the only place it can live.
                    if (common->typeTypeOrVoid()
                            && common->typeTypeOrVoid()->CARET() != nullptr) {
                        method->setReturnsView(true);
                    }
                    // Interface methods are the TARGET of every override edge.
                    if (common->getStart()) {
                        method->setDeclPosition(
                            (int) common->getStart()->getLine(),
                            (int) common->getStart()->getCharPositionInLine());
                    }
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

        // Parse `@SuppressLint(...)`'s argument text into lint-rule IDs, accepting a
        // single string literal or an array initializer. Escape sequences are not
        // supported — lint IDs are ASCII kebab-case by convention.
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


        // Visit one class-body member: nested type declarations become a no-op
        // NestedClassDeclaration, annotations and modifiers are attached, a bodyless
        // annotated method may gain a synthesized body, and operators are post-checked.
        virtual std::any visitClassBodyDeclaration(CajetaParser::ClassBodyDeclarationContext* ctx) override {
            // A nested classDeclaration returns a CajetaClassPtr, which cannot cast
            // to MemberDeclarationPtr; wrap it so the body iteration stays typed.
            if (auto memberDecl = ctx->memberDeclaration()) {
                if (memberDecl->classDeclaration()) {
                    std::any innerAny = visitClassDeclaration(memberDecl->classDeclaration());
                    CajetaClassPtr inner;
                    try {
                        inner = std::any_cast<CajetaClassPtr>(innerAny);
                    } catch (...) {
                        // Return shape changed: fall through to a null wrapper.
                    }
                    return std::static_pointer_cast<MemberDeclaration>(
                        std::make_shared<NestedClassDeclaration>(
                            inner, ctx->getStart()));
                }
                // Nested interfaces / enums / annotation types: unsupported, but must
                // not crash the any_cast.
                if (memberDecl->interfaceDeclaration()
                        || memberDecl->enumDeclaration()
                        || memberDecl->annotationTypeDeclaration()) {
                    visitChildren(memberDecl);
                    return std::static_pointer_cast<MemberDeclaration>(
                        std::make_shared<NestedClassDeclaration>(
                            nullptr, ctx->getStart()));
                }
            }
            // A bare `;` or a `STATIC? block` initializer has no memberDeclaration,
            // and visitChildren(nullptr) would SIGSEGV.
            if (!ctx->memberDeclaration()) {
                return std::static_pointer_cast<MemberDeclaration>(
                    std::make_shared<NestedClassDeclaration>(
                        nullptr, ctx->getStart()));
            }
            MemberDeclarationPtr memberDeclaration = any_cast<MemberDeclarationPtr>(visitMemberDeclaration(
                ctx->memberDeclaration()));
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

            // Declaration-time body synthesis: a bodyless annotated method offers its
            // declaration to the body registry, and a claiming synthesizer's `{ ... }`
            // is spliced over the `;` and re-visited. The re-visit cannot recurse.
            if (auto methodDecl = std::dynamic_pointer_cast<MethodDeclaration>(memberDeclaration)) {
                auto m = methodDecl->getMethod();
                if (m && m->isAbstract() && !m->getAnnotationInstances().empty()) {
                    cajeta::synth::registerBuiltinSynthesizers();
                    cajeta::synth::SynthesisContext sctx;
                    sctx.parent = pModule->getStructureStack().empty()
                        ? nullptr : pModule->getStructureStack().back();
                    sctx.module = pModule;
                    sctx.methodName = m->getName();
                    for (auto& p : m->getParameterList()) {
                        if (p && p->getName() != "this") {
                            sctx.paramTypes.push_back(p->getType());
                        }
                    }
                    sctx.method = m;
                    if (auto body = cajeta::synth::SynthesizerRegistry::instance()
                            .dispatchBody(sctx)) {
                        auto* startTok = ctx->getStart();
                        auto* stopTok = ctx->getStop();
                        if (startTok && stopTok && startTok->getInputStream()) {
                            antlr4::misc::Interval interval(
                                startTok->getStartIndex(), stopTok->getStopIndex());
                            std::string declText =
                                startTok->getInputStream()->getText(interval);
                            auto semi = declText.rfind(';');
                            if (semi != std::string::npos) {
                                declText = declText.substr(0, semi) + " " + *body;
                                if (const char* dump = std::getenv("CAJETA_DUMP_IR")) {
                                    if (dump[0] == '1') {
                                        std::cerr << "[Synthesizer] body for "
                                            << m->getName() << ":\n"
                                            << declText << "\n";
                                    }
                                }
                                xref::SyntheticSourceScope xrefMask;
                                auto* frag = cajeta::synth::parseClassBodyFragment(
                                    "{ " + declText + " }");
                                for (auto* cbd : frag->classBodyDeclaration()) {
                                    return visitClassBodyDeclaration(cbd);
                                }
                            }
                        }
                    }
                }
            }

            // Post-checks that need the modifier walk above to have stamped STATIC:
            // per-category operator staticness and arity (kStaticOps / kInstanceOps
            // below), and the final-or-static rule on method-level templates.
            if (auto methodDecl = std::dynamic_pointer_cast<MethodDeclaration>(memberDeclaration)) {
                if (auto m = methodDecl->getMethod()) {
                    const std::string& name = m->getName();
                    bool isStatic = m->getModifiers().find(STATIC)
                                  != m->getModifiers().end();
                    size_t arity = m->getParameterList().size();

                    // Must-be-static operators: name -> (minArity, maxArity).
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
                    // Must-be-instance operators: name -> expected param count.
                    static const std::unordered_map<std::string, size_t>
                        kInstanceOps = {
                            {"operator++",  0}, {"operator--",  0},
                            {"operator[]",  1}, {"operator[]=", 2},
                            {"operator#[]", 1},
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
                        // A @ValueType receiver is a by-value copy, so a mutating
                        // operator would write the copy; read-only `operator[]` is
                        // exempt.
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

        // Build a Method named `operator<symbol>` so BinaryOpExpression and
        // PrefixExpression can look an overload up like any other method. The receiver
        // is implicit, as for any non-static method.
        virtual std::any visitOperatorOverloadDeclaration(CajetaParser::OperatorOverloadDeclarationContext *ctx) override {
            const char* sym = "?";
            // Bracket forms first: `OPERATOR LBRACK RBRACK (ASSIGN)?` overlaps the
            // bare ASSIGN check below, so the more specific match must win.
            if (ctx->REFERENCE() && ctx->LBRACK() && ctx->RBRACK()) sym = "#[]";
            else if (ctx->LBRACK() && ctx->RBRACK() && ctx->ASSIGN()) sym = "[]=";
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
            // Every alternative uses `typeTypeOrVoid` for the return slot, so an
            // operator can declare a concrete return, `#T`, or `void`.
            CajetaTypePtr returnType;
            if (ctx->typeTypeOrVoid()) {
                returnType = CajetaType::fromContext(ctx->typeTypeOrVoid(), pModule);
                if (!returnType) {
                    // A null return type segfaults in generatePrototype; diagnose here.
                    reportOrThrow(ctx->typeTypeOrVoid()->getStart(),
                        "CAJETA_ERROR_UNRESOLVED_TYPE",
                        "unresolved type '" + ctx->typeTypeOrVoid()->getText()
                            + "' in return type of operator '" + methodName + "'");
                    returnType = CajetaType::error();
                }
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
                // .back() = the innermost class on the stack, which is the
                // correct parent for a method inside a nested class.
                pModule->getStructureStack().back());
            // The `operator` keyword is this declaration's name token. Operators must
            // appear in the export — cajetadoc's model omits them entirely.
            if (ctx->OPERATOR() && ctx->OPERATOR()->getSymbol()) {
                method->setDeclPosition(
                    (int) ctx->OPERATOR()->getSymbol()->getLine(),
                    (int) ctx->OPERATOR()->getSymbol()->getCharPositionInLine());
            }
            // `#T operator+ (...)` — the return transfers ownership.
            if (ctx->typeTypeOrVoid()
                    && ctx->typeTypeOrVoid()->REFERENCE() != nullptr) {
                method->setReturnsOwnership(true);
            }
            // `^T operator[] (...)` — the VIEW stance, the `keyAt` shape: an indexer
            // handing back interior state.
            if (ctx->typeTypeOrVoid()
                    && ctx->typeTypeOrVoid()->CARET() != nullptr) {
                // ...but never on `operator#[]`, which exists to extract a title OUT
                // of the container; `^` belongs on the plain `operator[]`.
                if (ctx->REFERENCE() && ctx->LBRACK() && ctx->RBRACK()) {
                    throw Exception(
                        "`operator#[]` cannot declare a `^` (view) return: it "
                        "is the title-EXTRACTING index operator — `#w[i]` "
                        "dispatches to it precisely to take ownership out of "
                        "the container, which is the opposite of a view. "
                        "Declare the view on the plain `operator[]` instead, "
                        "and keep `operator#[]` returning `#`. See "
                        "specs/stdlib-ownership-convention-spec.md §4.7.",
                        "CAJETA_ERROR_VIEW_ON_EXTRACTING_OPERATOR");
                }
                method->setReturnsView(true);
            }
            return static_pointer_cast<MemberDeclaration>(
                make_shared<MethodDeclaration>(method, ctx->getStart()));
        }

        // Build a Method from a method declaration. Method-level type parameters push
        // a placeholder substitution so formals and the return type resolve, and the
        // body parse is deferred to the per-call monomorphization re-parse.
        virtual std::any visitMethodDeclaration(CajetaParser::MethodDeclarationContext* ctx) override {
            string name = ctx->identifier()->getText();

            vector<TypeParameter> methodTypeParameters;
            bool isMethodTemplate = false;
            if (auto* tps = ctx->typeParameters()) {
                isMethodTemplate = true;
                for (auto* tp : tps->typeParameter()) {
                    TypeParameter param(tp->identifier()->getText());
                    if (tp->REFERENCE() != nullptr) {
                        throw Exception(
                            "`#` on a type parameter declaration is retired: "
                            "ownership is per-call under title-tracking "
                            "(specs/title-tracking-spec.md §8.1) — drop the "
                            "`#` (a must-own edge is spelled on the FORMAL)",
                            "CAJETA_ERROR_TYPE_TRANSFER_RETIRED");
                    }
                    param.owningRequired = false;
                    if (auto* pt = tp->primitiveType()) {
                        // Non-type (integer-constant) parameter, e.g. `<uint32 N>`.
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
            // Push a placeholder substitution: each method-level T-var gets a fresh
            // placeholder class. If the first name already resolves, this is a
            // monomorphization re-parse — keep the real bindings and DO walk the body.
            bool isInstantiationReparse = false;
            if (isMethodTemplate && !methodTypeParameters.empty()) {
                if (pModule->lookupTypeParameter(
                        methodTypeParameters[0].name)) {
                    isInstantiationReparse = true;
                }
            }
            if (isMethodTemplate && !isInstantiationReparse) {
                // lookupTypeParameter only checks the TOP frame, so carry any
                // class-level bindings forward into this one.
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
            // Stamp T-var-typed formals with the declared type-parameter name while
            // it is knowable: the resolved type OBJECT is later mutable (placeholder
            // fill can repoint it), this stamp is not.
            if (isMethodTemplate) {
                if (auto frame = pModule->getCurrentTypeSubstitution()) {
                    for (auto& fp : formalParameters) {
                        if (!fp || !fp->getType()) continue;
                        for (auto& tp : methodTypeParameters) {
                            auto b = frame->find(tp.name);
                            if (b != frame->end()
                                    && b->second == fp->getType()) {
                                fp->setDeclaredTypeParamName(tp.name);
                                break;
                            }
                        }
                    }
                }
            }
            CajetaTypePtr returnType = CajetaType::fromContext(ctx->typeTypeOrVoid(), pModule);
            if (ctx->typeTypeOrVoid() != nullptr && !returnType) {
                // A null return type reaches llvm::FunctionType::get(nullptr, ...)
                // and segfaults instead of diagnosing.
                reportOrThrow(ctx->typeTypeOrVoid()->getStart(),
                    "CAJETA_ERROR_UNRESOLVED_TYPE",
                    "unresolved type '" + ctx->typeTypeOrVoid()->getText()
                        + "' in return type of method '" + name + "'");
                returnType = CajetaType::error();  // recover: analysis continues
            }
            // methodBody is `block` or `;`; the `;` form yields an empty std::any, so
            // guard the cast. A method template's body parse is deferred to the
            // instantiation re-parse, where real arg types are bound.
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
                // .back() = the innermost class, the correct parent for a
                // method inside a nested class.
                pModule->getStructureStack().back());
            // Point at the method's NAME token — what an IDE navigates to and renames.
            if (ctx->identifier() && ctx->identifier()->getStart()) {
                method->setDeclPosition(
                    (int) ctx->identifier()->getStart()->getLine(),
                    (int) ctx->identifier()->getStart()->getCharPositionInLine());
            }
            if (isMethodTemplate) {
                method->setMethodTypeParameters(std::move(methodTypeParameters));
                // Source-text capture happens in visitClassBodyDeclaration, where the
                // enclosing final/static modifiers are in scope.
            }
            method->setVarargs(varargs);
            // No body means abstract, which gates function emission and the
            // override-vs-introduce decision in buildVirtualTable. A method template
            // also lands with a null block but is NOT abstract.
            if (!block && !isMethodTemplate) {
                method->setAbstract(true);
            }
            // `#T foo()` — the return transfers ownership; the grammar puts the `#`
            // on typeTypeOrVoid.
            if (ctx->typeTypeOrVoid() && ctx->typeTypeOrVoid()->REFERENCE() != nullptr) {
                method->setReturnsOwnership(true);
            }
            // `^T foo()` — the VIEW stance; one optional prefix, so `#` and `^`
            // can never both be set.
            if (ctx->typeTypeOrVoid() && ctx->typeTypeOrVoid()->CARET() != nullptr) {
                method->setReturnsView(true);
            }
            // `throws T1, T2` — an advisory list carried for the lint pass; no
            // enforcement here.
            if (auto* qnList = ctx->qualifiedNameList()) {
                vector<QualifiedNamePtr> throws;
                for (auto* qn : qnList->qualifiedName()) {
                    throws.push_back(QualifiedName::fromContext(qn));
                }
                method->setThrowsList(std::move(throws));
            }
            return static_pointer_cast<MemberDeclaration>(make_shared<MethodDeclaration>(method, ctx->getStart()));
        }

        /** Returns the method's Block inside an Any (empty for the `;` form).
         *  Prototype discovery stops short of bodies so every CU's prototypes are
         *  defined before any method definition is processed. */
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
            // The constructor's name token — what an IDE navigates to.
            if (ctx->identifier() && ctx->identifier()->getStart()) {
                method->setDeclPosition(
                    (int) ctx->identifier()->getStart()->getLine(),
                    (int) ctx->identifier()->getStart()->getCharPositionInLine());
            }
            if (auto* qnList = ctx->qualifiedNameList()) {
                vector<QualifiedNamePtr> throws;
                for (auto* qn : qnList->qualifiedName()) {
                    throws.push_back(QualifiedName::fromContext(qn));
                }
                method->setThrowsList(std::move(throws));
            }
            return static_pointer_cast<MemberDeclaration>(make_shared<MethodDeclaration>(method, ctx->getStart()));
        }

        // `~ClassName() { ... }`. Builds the body as a method internally named "drop",
        // which CajetaClass::getOrCreateDropFunction picks up unchanged; the
        // identifier must match the enclosing class name.
        virtual std::any visitDestructorDeclaration(CajetaParser::DestructorDeclarationContext* ctx) override {
            string declaredName = ctx->identifier()->getText();
            auto enclosing = pModule->getStructureStack().back();
            string className = enclosing
                ? enclosing->getQName()->getTypeName()
                : string();
            // A generic class's type name is monomorphized ("Buffer<float32>") but the
            // destructor is written `~Buffer()`, so compare against the base name.
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
            // fromContext returns a placeholder for a type the archive knows but has
            // not visited, and throws for a name declared nowhere.
            if (!type) {
                // Anchor on the TYPE token: an unlocated Exception leaves
                // hasLocation() false, and every consumer then reports it at line 1.
                string typeName = ctx->typeType()->getText();
                string declared = ctx->variableDeclarators()->getText();
                throw cajeta::locatedException(
                    ctx->typeType()->getStart(),
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
                // `T x #= v` wraps the initializer in a MoveExpression. It is
                // MODE-CARRYING, not an unconditional transfer: a name or slot source
                // forwards the title it actually holds, so a lend stays a lend.
                if (ctx->SHARP_ASSIGN() != nullptr
                        && ctx->variableInitializer()->expression() != nullptr) {
                    // `T x #= #v` — the transfer spelled twice; `#=` already carries
                    // the source's mode, so the second `#` restates it.
                    bool redundant = cajeta::cajetaRhsCarriesRedundantSharp(
                        ctx->variableInitializer()->expression());
                    auto inner = any_cast<ExpressionPtr>(
                        visitExpression(ctx->variableInitializer()->expression()));
                    // Warned, not rejected — reported from MoveExpression::generateCode.
                    if (redundant) {
                        if (auto redMv = dynamic_pointer_cast<
                                cajeta::MoveExpression>(inner)) {
                            redMv->setRedundantSharp(true);
                        }
                    }
                    auto mv = make_shared<MoveExpression>(
                        ctx->variableInitializer()->getStart());
                    mv->setSharpStore(true);
                    mv->addChild(inner);
                    initializer = make_shared<VariableInitializer>(
                        mv, ctx->variableInitializer()->getStart());
                } else {
                    initializer = any_cast<InitializerPtr>(visitVariableInitializer(ctx->variableInitializer()));
                    // The legacy `T x = #v` form; the SHARP_ASSIGN branch above is the
                    // new spelling and stays quiet.
                    markLegacyTransferAssign(initializer);
                }
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
            // Return as InitializerPtr: std::any keys on the exact type, so a
            // shared_ptr<ArrayInitializer> would fail the caller's any_cast.
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

        // v1: register an `annotation` declaration as a minimal, layout-less type so
        // it resolves as a type token (`classesAnnotated<@A>()`); body elements stay
        // inert and it never enters `structures`, keeping it off the codegen worklist.
        virtual std::any
        visitAnnotationTypeDeclaration(CajetaParser::AnnotationTypeDeclarationContext* ctx) override {
            const std::string annName = ctx->identifier()->getText();
            QualifiedNamePtr qName = QualifiedName::getOrInsert(
                annName, pModule->getQName()->getPackageName());
            list<QualifiedNamePtr> none;
            auto ann = make_shared<CajetaClass>(pModule, qName, none, none);
            ann->setIsAnnotation(true);
            auto& canon = CajetaType::getCanonicalMap();
            // The canonicalMap KEY stays the collision-safe "code" pseudo-package:
            // keying by the real FQN would clobber a same-named real class, which
            // would then resolve to this layout-less type and SIGSEGV at allocation.
            QualifiedNamePtr codeKey = QualifiedName::getOrInsert(annName, "code");
            canon[codeKey->toCanonical()] = static_pointer_cast<CajetaType>(ann);

            // Never in `structures`, so the export sees only this canonicalMap entry:
            // with no stamped position it is unplaceable and silently absent.
            ann->setDeclaringFile(pModule->currentSourceFile());
            if (ctx->identifier() && ctx->identifier()->getStart()) {
                ann->setDeclPosition(
                    (int) ctx->identifier()->getStart()->getLine(),
                    (int) ctx->identifier()->getStart()->getCharPositionInLine());
            }
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
            if (ctx->REFERENCE() != nullptr) {
                throw Exception(
                    "`#` on a local declaration's type is retired: a local's "
                    "role comes from its initializer under title-tracking "
                    "(specs/title-tracking-spec.md §8.1) — drop the `#` from "
                    "the declaration (the initializer's `#x` / owned rvalue "
                    "already carries the title)",
                    "CAJETA_ERROR_TYPE_TRANSFER_RETIRED");
            }
            auto* typeCtx = ctx->typeType();
            CajetaTypePtr declType = CajetaType::fromContext(typeCtx, pModule);
            // Only an EXPLICIT type that resolves to null is an error (`var` has a
            // null typeType); a null type would SIGSEGV at the first deref.
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

        // `arrayLiteral : '[' expressionList? ']'` (XPU launch dims). The AST is built
        // by Expression::fromContext, so this entry only descends.
        virtual std::any visitArrayLiteral(CajetaParser::ArrayLiteralContext* ctx) override {
            return visitChildren(ctx);
        }

        // The entry list and each entry are consumed directly by
        // arrayOrMapLiteralFromContext; these hooks only keep the visitor concrete.
        virtual std::any visitArrayLiteralEntries(
                CajetaParser::ArrayLiteralEntriesContext* ctx) override {
            return visitChildren(ctx);
        }

        virtual std::any visitArrayLiteralEntry(
                CajetaParser::ArrayLiteralEntryContext* ctx) override {
            return visitChildren(ctx);
        }

        // Expression::fromContext builds the whole sub-tree by its own recursive
        // descent; a visitor-driven addChild loop here breaks postfix operators.
        virtual std::any visitExpression(CajetaParser::ExpressionContext* ctx) override {
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
            // `Foo { field: expr, ... }`. The node and codegen live in
            // AggregateInitializerExpression; this visit descends into nested exprs.
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
