// CajetaClass::instantiate — materialize a concrete class from a template by
// re-parsing its captured source under a type-parameter substitution. Kept in
// its own TU so CajetaClass.h need not drag in the visitor + parser machinery.

#include "CajetaArray.h"
#include "CajetaClass.h"
#include "QualifiedName.h"
#include "../asn/ClassBodyDeclaration.h"
#include "../compile/CajetaModule.h"
#include "../compile/CajetaLlvmVisitor.h"
#include "../compile/Compiler.h"
#include "../error/Exception.h"
#include "cajeta/xref/XrefIndex.h"
#include "CajetaParser.h"
#include "CajetaLexer.h"

#include "antlr4-runtime/antlr4-runtime.h"

#include <cassert>

namespace cajeta {

    // Build `<arg0,arg1,...>` from the args' canonical names — the suffix of
    // the instantiation's class name and the structure-map cache key.
    static string buildArgSuffix(const vector<CajetaTypePtr>& args) {
        string s = "<";
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) s += ",";
            s += args[i]->getQName()->toCanonical();
        }
        s += ">";
        return s;
    }

    // Rebuild the module's `package` / `import` preamble so a re-parsed
    // template snippet resolves the same names its body was written against.
    static string synthesizePreamble(CajetaModulePtr module) {
        string out;
        const string& pkg = module->getQName()->getPackageName();
        if (!pkg.empty()) {
            out += "package " + pkg + ";\n";
        }
        for (auto& byType : module->getImports()) {
            const string& typeName = byType.first;
            for (auto& byPkg : byType.second) {
                const string& pkgName = byPkg.first;
                out += "import ";
                if (!pkgName.empty()) {
                    out += pkgName + ".";
                }
                out += typeName + ";\n";
            }
        }
        return out;
    }

    vector<CajetaClass::DeferredInstantiation>&
    CajetaClass::deferredInstantiations() {
        static thread_local vector<DeferredInstantiation> pending;
        return pending;
    }

    // Instantiation cost, split into the ANTLR body re-walk vs LLVM lowering.
    static long long g_walkNs = 0, g_protoNs = 0, g_instCount = 0;

    CajetaClassPtr& CajetaClass::instantiationReuseTarget() {
        static thread_local CajetaClassPtr target;
        return target;
    }

    void CajetaClass::resetDeferredInstantiationState() {
        deferredInstantiations().clear();
        instantiationReuseTarget().reset();
    }

    // Completes every queued instantiation whose template has since materialized, and
    // returns true if any progressed, so the caller's fixpoint re-runs. Index-based: a
    // completion appends entries mid-iteration, and an unwind clears the whole queue.
    bool CajetaClass::drainDeferredInstantiations() {
        auto& pending = deferredInstantiations();
        if (pending.empty()) return false;
        bool progressed = false;
        // Index-based: completing one instantiation can append more entries
        // while we iterate. On unwind (a member synthesizer's USER diagnostic)
        // the queue holds shared_ptrs into THIS compile, so clear and restore.
        struct DrainUnwindGuard {
            vector<DeferredInstantiation>& pending;
            CajetaClassPtr saved;
            bool dismissed = false;
            ~DrainUnwindGuard() {
                if (dismissed) return;
                instantiationReuseTarget() = saved;
                pending.clear();
            }
        } guard{pending, instantiationReuseTarget()};
        const bool diTiming = std::getenv("CAJETA_PRIME_TIMING") != nullptr;
        const auto diStart = std::chrono::steady_clock::now();
        long long diSeen = 0, diRan = 0;
        long long diWorstNs = 0;
        size_t diWorstMethods = 0;
        std::string diWorst;
        for (size_t i = 0; i < pending.size(); ++i) {
            auto& d = pending[i];
            ++diSeen;
            if (!d.templateClass || !d.target) continue;
            if (d.templateClass->isPlaceholder()) continue;
            if (!d.target->isPlaceholder()) continue;
            CajetaClassPtr& reuse = instantiationReuseTarget();
            CajetaClassPtr saved = reuse;
            reuse = d.target;
            const auto diOne = std::chrono::steady_clock::now();
            d.templateClass->instantiateInternal(d.args);
            if (diTiming) {
                auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - diOne).count();
                ++diRan;
                if (ns > diWorstNs) {
                    diWorstNs = ns;
                    diWorst = d.templateClass->getQName()
                        ? d.templateClass->getQName()->toCanonical() : "?";
                    diWorstMethods = d.target ? d.target->getMethods().size() : 0;
                }
            }
            reuse = saved;
            if (!d.target->isPlaceholder()) progressed = true;
        }
        if (diTiming) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - diStart).count();
            if (ms > 50) {
                std::fprintf(stderr,
                    "[defer] %lld ms  seen=%lld ran=%lld  worst=%lld ms (%s: "
                    "%zu methods)  walk=%lld ms proto=%lld ms insts=%lld\n",
                    (long long) ms, diSeen, diRan, diWorstNs / 1000000,
                    diWorst.c_str(), diWorstMethods,
                    g_walkNs / 1000000, g_protoNs / 1000000, g_instCount);
            }
        }
        guard.dismissed = true;
        pending.erase(std::remove_if(pending.begin(), pending.end(),
            [](const DeferredInstantiation& d) {
                return !d.target || !d.target->isPlaceholder();
            }), pending.end());
        return progressed;
    }

    // instantiateInternal, plus the cross-module obligation: a result distinct from
    // `this` is a real instantiation the current codegen module has to be told about.
    CajetaClassPtr CajetaClass::instantiate(vector<CajetaTypePtr> args) {
        CajetaClassPtr result = instantiateInternal(std::move(args));
        // Only a genuine instantiation (a distinct object from the template)
        // is a cross-module obligation; the note no-ops outside codegen.
        if (result && result.get() != this) {
            CajetaModule::noteCrossModuleInstantiation(
                CajetaModule::getCurrentCodegenModule(), result);
        }
        return result;
    }

    // The instantiation proper: fill trailing defaults, short-circuit on placeholder or
    // bare-template args, check arity and bounds, then reuse the cached instantiation or
    // re-parse the captured body under the substitution. Returns `this` if not a template.
    CajetaClassPtr CajetaClass::instantiateInternal(vector<CajetaTypePtr> args) {
        if (!isTemplate()) {
            return static_pointer_cast<CajetaClass>(shared_from_this());
        }

        // Fill omitted TRAILING parameters from their declared defaults before
        // the cache key, so `Foo` and `Foo<float32>` share one identity; an
        // empty default stops the fill and the arity check rejects the gap.
        if (args.size() < typeParameters.size()) {
            for (size_t i = args.size(); i < typeParameters.size(); ++i) {
                const auto& p = typeParameters[i];
                if (p.defaultType.empty()) break;
                auto it = CajetaType::canonicalMap.find(p.defaultType);
                if (it == CajetaType::canonicalMap.end() || !it->second) {
                    throw Exception(
                        "template " + qName->toCanonical() + ": default type '"
                            + p.defaultType + "' for parameter '" + p.name
                            + "' is unresolved",
                        "CAJETA_ERROR_TYPE_PARAMETER_DEFAULT");
                }
                args.push_back(it->second);
            }
        }

        // Placeholder args: a T-var placeholder (empty package, from a method
        // template) must never be baked into the cache, while a forward-ref
        // placeholder (real package) is filled in place later — let it through.
        for (auto& arg : args) {
            if (auto cls = dynamic_pointer_cast<CajetaClass>(arg)) {
                if (cls->isPlaceholder()) {
                    bool isTVar = !cls->getQName()
                        || cls->getQName()->getPackageName().empty();
                    // Interfaces keep short-circuiting on any placeholder:
                    // vtable conformance is signature-strict and would reject
                    // a static-method @Encoding implementer.
                    if (isTVar || interfaceFlag) {
                        return static_pointer_cast<CajetaClass>(
                            shared_from_this());
                    }
                }
                // A bare (uninstantiated) template arg is the trail of a
                // deeper short-circuit; building over it starts a cascade
                // whose codegen has no LLVM type for the raw arg.
                if (cls->isTemplate() && !cls->getTypeParameters().empty()) {
                    return static_pointer_cast<CajetaClass>(
                        shared_from_this());
                }
            }
        }

        if (args.size() != typeParameters.size()) {
            throw Exception(
                "template " + qName->toCanonical() + " expects "
                    + std::to_string(typeParameters.size())
                    + " type argument(s), got " + std::to_string(args.size()),
                "CAJETA_ERROR_TYPE_PARAMETER_ARITY");
        }

        // Constraint enforcement: each `<T extends Bound>` is checked by
        // walking the argument's supertype chain. Primitives carry no chain
        // and are rejected — bounds demand a class/interface relationship.
        for (size_t i = 0; i < typeParameters.size(); ++i) {
            const auto& param = typeParameters[i];
            bool argIsConst = args[i] && (args[i]->getTypeFlags() & CONSTANT_FLAG);
            if (param.isNonType && !argIsConst) {
                throw Exception(
                    "template " + qName->toCanonical() + ": non-type parameter '"
                        + param.name + "' (" + param.nonTypePrimitive
                        + ") requires an integer-constant argument",
                    "CAJETA_ERROR_TYPE_PARAMETER_KIND");
            }
            if (!param.isNonType && argIsConst) {
                throw Exception(
                    "template " + qName->toCanonical() + ": type parameter '"
                        + param.name + "' cannot take an integer-constant argument",
                    "CAJETA_ERROR_TYPE_PARAMETER_KIND");
            }
            if (param.bounds.empty()) continue;
            // An unbounded `?` stands for some T that fits, so any bound holds.
            if (args[i] && args[i]->isWildcard()) continue;
            auto argClass = dynamic_pointer_cast<CajetaClass>(args[i]);
            for (auto& bound : param.bounds) {
                // Numeric/Integral/Floating/Complex are marker bounds with dual
                // conformance: a primitive satisfies intrinsically (type-flag
                // lattice), a class nominally (`implements`).
                const std::string& bname = bound->getTypeName();
                if (CajetaClass::isNumericMarkerName(bname)) {
                    if (!CajetaClass::satisfiesNumericMarker(args[i], bname)) {
                        std::string argName = (args[i] && args[i]->getQName())
                            ? args[i]->getQName()->toCanonical()
                            : std::string("?");
                        throw Exception(
                            "template " + qName->toCanonical() + ": argument '"
                                + argName + "' does not satisfy bound "
                                + bname + " on parameter '" + param.name
                                + "' — " + bname + " admits a primitive "
                                + (bname == "Floating"
                                    ? "floating-point type (float16, bfloat16, "
                                      "float32, float64, float128, or a "
                                      "low-precision float8/float6/float4 format)"
                                    : (bname == "Integral"
                                        ? "integer type (int8..int128, "
                                          "uint8..uint128)"
                                        : "numeric type (any integer or "
                                          "floating-point primitive)"))
                                + ", or a class implementing cajeta.lang." + bname,
                            "CAJETA_ERROR_TYPE_PARAMETER_BOUND");
                    }
                    continue;
                }
                // canonicalMap holds a template under both its canonical and
                // its short name; `extends` lookups try the canonical first.
                auto& cmap = CajetaType::getCanonicalMap();
                CajetaTypePtr boundType;
                auto it = cmap.find(bound->toCanonical());
                if (it != cmap.end()) {
                    boundType = it->second;
                } else {
                    auto nit = cmap.find(bound->getTypeName());
                    if (nit != cmap.end()) boundType = nit->second;
                }
                auto boundClass = dynamic_pointer_cast<CajetaClass>(boundType);
                if (!boundClass) {
                    throw Exception(
                        "template " + qName->toCanonical() + ": bound '"
                            + bound->getTypeName() + "' on parameter '"
                            + param.name + "' did not resolve to a class",
                        "CAJETA_ERROR_TYPE_PARAMETER_BOUND");
                }
                if (!argClass || !argClass->isParentOrKind(boundClass)) {
                    throw Exception(
                        "template " + qName->toCanonical() + ": argument '"
                            + args[i]->getQName()->toCanonical()
                            + "' does not satisfy bound '"
                            + bound->getTypeName() + "' on parameter '"
                            + param.name + "'",
                        "CAJETA_ERROR_TYPE_PARAMETER_BOUND");
                }
            }
        }

        // Cache key: full canonical name with args, `pkg.Box<cajeta.int32>`.
        string suffix = buildArgSuffix(args);
        string instCanonical = qName->toCanonical() + suffix;

        // Resolution runs against `module` (the template's own imports and
        // substitution); IR EMITS into `emitOwner`, which must be picked now
        // because methods adopt it at construction, mid-walk.
        CajetaModulePtr emitOwner = module;
        if (Compiler::getSharedContext()
                && module == CajetaModule::getStdlibModule()) {
            for (auto& arg : args) {
                auto argClass = dynamic_pointer_cast<CajetaClass>(arg);
                if (argClass && argClass->getEmitModule()
                        && argClass->getEmitModule() != module) {
                    emitOwner = argClass->getEmitModule();
                    break;
                }
            }
            // In a notebook the declaring unit is already sealed in the JIT and
            // can never take delivery of new IR, so a USER-typed specialization
            // moves to the unit being compiled; pure-stdlib ones must not.
            if (emitOwner != module && CajetaModule::getActiveUnitModule()) {
                auto active = CajetaModule::sessionEmitTarget();
                if (active && active != module) emitOwner = active;
            }
            if (emitOwner == module
                    && CajetaModule::getCurrentCodegenModule()
                    && CajetaModule::getCurrentCodegenModule() != module) {
                emitOwner = CajetaModule::getCurrentCodegenModule();
            }
            if (emitOwner == module
                    && CajetaModule::getReuseEmitModule()
                    && CajetaModule::getReuseEmitModule() != module) {
                emitOwner = CajetaModule::getReuseEmitModule();
            }
        }

        auto& structures = module->getStructures();
        // While DRAINING, the canonical name already maps to the placeholder we
        // came to fill, so the caches must be bypassed or nothing progresses.
        const bool drainingThis = (bool) instantiationReuseTarget();
        auto cached = structures.find(instCanonical);
        if (cached == structures.end()) ++g_instCount;
        if (!drainingThis && cached != structures.end()) {
            return cached->second;
        }
        // Process-wide: one instantiation per canonical name across modules.
        // Without this a second module rebuilds the same `T<args>` and the
        // duplicate #RttiGlobal / #VTable definitions collide at link.
        {
            auto& structToMod = CajetaModule::getStructureToModule();
            auto stIt = structToMod.find(instCanonical);
            if (!drainingThis && stIt != structToMod.end() && stIt->second) {
                auto& owningStructures = stIt->second->getStructures();
                auto cachedGlobal = owningStructures.find(instCanonical);
                if (cachedGlobal != owningStructures.end()) {
                    return cachedGlobal->second;
                }
            }
        }

        // A NOVEL instantiation, past every cache. Under test reuse with
        // emitOwner != module its IR would leave module-bound llvm pointers on
        // persistent objects — abort unless CAJETA_REUSE_FORCE_EMIT overrides.
        if (Compiler::isReuseHazardArmed() && emitOwner != module) {
            static const bool kForceEmit =
                std::getenv("CAJETA_REUSE_FORCE_EMIT") != nullptr;
            if (!kForceEmit) {
                throw cajeta::ReuseHazardAbort{};
            }
        }

        // Templated interface: re-parse the captured interfaceDeclaration under
        // the substitution. visitInterfaceDeclaration has no standalone entry
        // point, so its body walk is duplicated inline below.
        if (interfaceFlag) {
            if (templateSource.empty()) {
                // No captured source (the verification-only @Encoding path):
                // hand the template back unchanged.
                return static_pointer_cast<CajetaClass>(shared_from_this());
            }

            // Snippet positions belong to no file: mask xref so the
            // instantiated body's callees are not attributed to the call site.
            xref::SyntheticSourceScope xrefMask;

            string ifInput = synthesizePreamble(module) + templateSource + "\n";
            antlr4::ANTLRInputStream ifStream(ifInput);
            CajetaLexer ifLexer(&ifStream);
            antlr4::CommonTokenStream ifTokens(&ifLexer);
            ifTokens.fill();
            CajetaParser ifParser(&ifTokens);
            auto* ifUnit = dynamic_cast<CajetaParser::CompilationUnitContext*>(
                parseSyntheticCompilationUnit(ifParser, ifTokens,
                                              "synthetic:interface"));
            CajetaParser::InterfaceDeclarationContext* ifDecl = nullptr;
            for (auto* td : ifUnit->typeDeclaration()) {
                if (auto* id = td->interfaceDeclaration()) {
                    ifDecl = id;
                    break;
                }
            }
            if (!ifDecl) {
                throw "template snippet does not contain an interfaceDeclaration";
            }

            string ifInstName = qName->getTypeName() + suffix;
            QualifiedNamePtr ifInstQName = QualifiedName::getOrInsert(
                ifInstName, qName->getPackageName());

            map<string, CajetaTypePtr> ifSubst;
            for (size_t i = 0; i < typeParameters.size(); ++i) {
                ifSubst[typeParameters[i].name] = args[i];
            }
            module->pushTypeSubstitution(ifSubst);
            auto prevActive = CajetaModule::getActiveModule();
            CajetaModule::setActiveModule(module);

            list<QualifiedNamePtr> ifExtended;
            list<QualifiedNamePtr> ifImplemented;
            if (auto* tl = ifDecl->typeList()) {
                for (auto* tt : tl->typeType()) {
                    auto* coi = tt->classOrInterfaceType();
                    if (!coi) continue;
                    CajetaTypePtr resolvedSuper = CajetaType::fromContext(tt, module);
                    auto superIface = dynamic_pointer_cast<CajetaClass>(resolvedSuper);
                    if (superIface) {
                        ifExtended.push_back(superIface->getQName());
                    } else {
                        ifExtended.push_back(QualifiedName::fromContext(coi));
                    }
                }
            }
            auto ifInst = make_shared<CajetaClass>(
                module, ifInstQName, ifExtended, ifImplemented);
            ifInst->setIsInterface(true);
            // The code lives in the template's file — frames must name it.
            ifInst->setDeclaringFile(getDeclaringFile());
            if (emitOwner != module) ifInst->setEmitModule(emitOwner);
            ifInst->setTypeParameters(typeParameters);
            ifInst->setTypeArguments(args);
            ifInst->setTemplateOrigin(
                static_pointer_cast<CajetaClass>(shared_from_this()));

            // Cache BEFORE walking so a self-reference resolves; registered
            // under emitOwner, plus structureToModule for the != module case.
            emitOwner->getStructures()[instCanonical] = ifInst;
            CajetaModule::getStructureToModule()[instCanonical] = emitOwner;

            auto ifBody = make_shared<ClassBodyDeclaration>(ifDecl->getStart());
            if (auto* body = ifDecl->interfaceBody()) {
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
                                if (auto p = FormalParameter::fromContext(fp, module)) {
                                    formals.push_back(p);
                                }
                            }
                        }
                    }
                    CajetaTypePtr returnType = CajetaType::fromContext(
                        common->typeTypeOrVoid(), module);
                    MethodPtr method = Method::create(
                        module, methodName, returnType, formals,
                        /*block=*/nullptr, ifInst);
                    method->setAbstract(true);
                    // This inline walk must carry `#` (transfer) and `^` (view)
                    // from typeTypeOrVoid as visitMethodDeclaration does, or a
                    // caller misclassifies an owned return as a borrow (leak).
                    if (common->typeTypeOrVoid()
                            && common->typeTypeOrVoid()->REFERENCE() != nullptr) {
                        method->setReturnsOwnership(true);
                    }
                    if (common->typeTypeOrVoid()
                            && common->typeTypeOrVoid()->CARET() != nullptr) {
                        method->setReturnsView(true);
                    }
                    ifBody->getDeclarations().push_back(
                        make_shared<MethodDeclaration>(method, common->getStart()));
                }
            }
            ifInst->setClassBody(ifBody);

            ifInst->generatePrototype();

            CajetaModule::setActiveModule(prevActive);
            module->popTypeSubstitution();

            CajetaModule::getStructureToModule()[instCanonical] = emitOwner;
            return ifInst;
        }

        // ANTLR contexts die with their parser, so the snippet is re-parsed on
        // demand; the cache above means that runs once per unique arg list.
        xref::SyntheticSourceScope xrefMask;

        string input = synthesizePreamble(module) + templateSource + "\n";

        antlr4::ANTLRInputStream inputStream(input);
        CajetaLexer lexer(&inputStream);
        antlr4::CommonTokenStream tokens(&lexer);
        tokens.fill();
        CajetaParser parser(&tokens);
        auto* compUnit = dynamic_cast<CajetaParser::CompilationUnitContext*>(
            parseSyntheticCompilationUnit(parser, tokens,
                                          "synthetic:instantiate"));

        CajetaParser::ClassDeclarationContext* classDecl = nullptr;
        CajetaParser::RecordDeclarationContext* recordDecl = nullptr;
        for (auto* td : compUnit->typeDeclaration()) {
            if ((classDecl = td->classDeclaration())) break;
            if ((recordDecl = td->recordDeclaration())) break;
        }
        if (!classDecl && !recordDecl) {
            throw "template snippet does not contain a classDeclaration";
        }

        string instName = qName->getTypeName() + suffix;
        QualifiedNamePtr instQName = QualifiedName::getOrInsert(
            instName, qName->getPackageName());

        // Push the substitution before resolving `extends`, or `T` in a
        // parameterized super fails to resolve and its args silently drop.
        map<string, CajetaTypePtr> subst;
        for (size_t i = 0; i < typeParameters.size(); ++i) {
            subst[typeParameters[i].name] = args[i];
        }
        module->pushTypeSubstitution(subst);
        // fromContext consults the active module for the substitution stack.
        auto prevActiveForSupers = CajetaModule::getActiveModule();
        CajetaModule::setActiveModule(module);

        // The grammar yields the extends / implements / permits typeLists in
        // source order with no tag: match each to the nearest preceding keyword.
        list<QualifiedNamePtr> instExtended;
        list<QualifiedNamePtr> instImplemented;
        list<vector<QualifiedNamePtr>> instImplementedTypeArgs;
        auto kwIdx = [](antlr4::tree::TerminalNode* n) -> ssize_t {
            return n && n->getSymbol() ? (ssize_t) n->getSymbol()->getTokenIndex() : -1;
        };
        ssize_t extKw = kwIdx(classDecl ? classDecl->EXTENDS() : recordDecl->EXTENDS());
        ssize_t implKw = classDecl ? kwIdx(classDecl->IMPLEMENTS()) : -1;
        ssize_t permKw = classDecl ? kwIdx(classDecl->PERMITS()) : -1;
        std::vector<CajetaParser::TypeListContext*> declTypeLists =
            classDecl ? classDecl->typeList() : recordDecl->typeList();
        for (auto* tl : declTypeLists) {
            ssize_t tlIdx = tl->getStart()
                ? (ssize_t) tl->getStart()->getTokenIndex() : -1;
            ssize_t best = -1;
            int which = -1; // 0=extends, 1=implements, 2=permits
            if (extKw >= 0 && extKw < tlIdx && extKw > best) { best = extKw; which = 0; }
            if (implKw >= 0 && implKw < tlIdx && implKw > best) { best = implKw; which = 1; }
            if (permKw >= 0 && permKw < tlIdx && permKw > best) { best = permKw; which = 2; }
            if (which < 0 || which == 2) continue;
            for (auto* tt : tl->typeType()) {
                auto* coi = tt->classOrInterfaceType();
                if (!coi) continue;
                if (which == 0) {
                    CajetaTypePtr resolvedSuper = CajetaType::fromContext(tt, module);
                    auto superClass = dynamic_pointer_cast<CajetaClass>(resolvedSuper);
                    if (superClass) {
                        instExtended.push_back(superClass->getQName());
                    } else {
                        instExtended.push_back(QualifiedName::fromContext(coi));
                    }
                } else { // implements
                    instImplemented.push_back(QualifiedName::fromContext(coi));
                    vector<QualifiedNamePtr> argsCaptured;
                    auto targsList = coi->typeArguments();
                    CajetaParser::TypeArgumentsContext* leafTargs = nullptr;
                    for (auto* ta : targsList) {
                        if (ta) leafTargs = ta;
                    }
                    if (leafTargs) {
                        for (auto* targ : leafTargs->typeArgument()) {
                            if (!targ || !targ->typeType()) continue;
                            auto* targTt = targ->typeType();
                            if (auto* targCoi = targTt->classOrInterfaceType()) {
                                string argName = targCoi->getText();
                                auto sit = subst.find(argName);
                                if (sit != subst.end() && sit->second
                                        && sit->second->getQName()) {
                                    argsCaptured.push_back(sit->second->getQName());
                                } else {
                                    argsCaptured.push_back(
                                        QualifiedName::fromContext(targCoi));
                                }
                            } else if (auto* targPrim = targTt->primitiveType()) {
                                argsCaptured.push_back(
                                    QualifiedName::getOrInsert(
                                        targPrim->getText(), ""));
                            }
                        }
                    }
                    instImplementedTypeArgs.push_back(std::move(argsCaptured));
                }
            }
        }
        CajetaModule::setActiveModule(prevActiveForSupers);

        // Instantiations are built straight from the parse tree, bypassing the
        // visitor's implicit `extends Object` injection; without it the class
        // is not `<: Object` and Object-parameter calls fail resolution.
        bool isObjectItself = instQName
            && instQName->getTypeName() == "Object"
            && instQName->getPackageName() == "cajeta.lang";
        if (instExtended.empty() && !isObjectItself) {
            instExtended.push_back(
                QualifiedName::getOrInsert("Object", "cajeta.lang"));
        }

        // DEFER while the template is itself a forward reference: building now
        // would derive the layout from fields nobody has parsed. Register a
        // placeholder under the canonical name, filled in place at drain.
        if (isPlaceholder() && !instantiationReuseTarget()) {
            auto ph = make_shared<CajetaClass>(module, instQName,
                                               instExtended, instImplemented);
            ph->setPlaceholder(true);
            module->getStructures()[instCanonical] = ph;
            CajetaModule::getStructureToModule()[instCanonical] = module;
            CajetaType::canonicalMap[instCanonical] =
                static_pointer_cast<CajetaType>(ph);
            DeferredInstantiation d;
            d.templateClass = static_pointer_cast<CajetaClass>(shared_from_this());
            d.args = args;
            d.target = ph;
            d.canonical = instCanonical;
            deferredInstantiations().push_back(d);
            // Pop the frame pushed for the supers resolution: leaking it buries
            // the CALLER's frame for the rest of its declaration walk.
            module->popTypeSubstitution();
            return ph;
        }

        CajetaClassPtr inst;
        if (CajetaClassPtr reuse = instantiationReuseTarget()) {
            // CONSUME it immediately: the body walk instantiates other
            // templates, and a target left set would make each of those bypass
            // its cache and try to fill this same object.
            instantiationReuseTarget() = nullptr;
            inst = reuse;
            inst->fillFromDeclaration(module, instQName, instExtended,
                                      instImplemented);
        } else {
            inst = make_shared<CajetaClass>(module, instQName, instExtended,
                                            instImplemented);
        }
        // The code lives in the template's file — frames must name it.
        inst->setDeclaringFile(getDeclaringFile());
        // Methods re-parsed from a synthetic snippet carry SNIPPET line
        // numbers. The snippet shifts uniformly, so one delta — the template's
        // true declLine minus the snippet's — corrects every method in it.
        if (declLine > 0 && classDecl && classDecl->getStart()) {
            const int snippetClassLine = (int) classDecl->getStart()->getLine();
            if (snippetClassLine > 0) {
                inst->setDbgLineDelta(declLine - snippetClassLine);
            }
        }
        if (recordDecl) inst->setRecordType(true);
        if (emitOwner != module) inst->setEmitModule(emitOwner);
        // The instantiation body is walked via visitClassBody, which never
        // re-runs visitClassDeclaration's annotation handling, so this is the
        // only path for class-level annotations (@ValueType layout, etc.).
        for (auto& ann : this->getAnnotationInstances()) {
            inst->addAnnotationInstance(ann);
        }
        inst->setQImplementedTypeArgs(std::move(instImplementedTypeArgs));
        inst->setTypeParameters(typeParameters);
        inst->setTypeArguments(args);
        inst->setTemplateOrigin(static_pointer_cast<CajetaClass>(shared_from_this()));
        // Modifiers are a property of the class shape, not the type arguments.
        // Without the copy, `final class ArrayList<T>` yields a non-final
        // instantiation whose `add()` stays vtable-dispatched.
        inst->getModifiers() = this->getModifiers();

        // Cache BEFORE the walk so a self-referential `List<T> next` resolves
        // against the partially-built instantiation instead of recursing.
        emitOwner->getStructures()[instCanonical] = inst;
        CajetaModule::getStructureToModule()[instCanonical] = emitOwner;

        // The visitor takes a method's parent from structureStack.front(), so
        // the walk needs a stack holding only `inst` — otherwise a class being
        // walked further out would adopt the template's methods.
        auto& stack = module->getStructureStack();
        list<CajetaClassPtr> savedStack;
        savedStack.swap(stack);
        stack.push_back(inst);

        auto prevActive = CajetaModule::getActiveModule();
        CajetaModule::setActiveModule(module);

        // The walk, the synthesizers and generatePrototype can all throw;
        // restore the substitution frame, active module and structure stack on
        // unwind, or a caught-and-continued compile runs with corrupt state.
        try {
        const auto walkT0 = std::chrono::steady_clock::now();
        CajetaLlvmVisitor visitor(module);
        auto bodyAny = visitor.visitClassBody(
            classDecl ? classDecl->classBody() : recordDecl->classBody());
        inst->setClassBody(std::any_cast<ClassBodyDeclarationPtr>(bodyAny));
        g_walkNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - walkT0).count();

        // The walk resolved field types through the substitution, so a bare `P`
        // field no longer knows it CAME FROM a type parameter: record the index
        // for the drop walk's T-origin branch (emitDropBodyInline).
        if (classDecl && classDecl->classBody()) {
            for (auto* bodyDecl : classDecl->classBody()->classBodyDeclaration()) {
                auto* member = bodyDecl->memberDeclaration();
                if (!member || !member->fieldDeclaration()) continue;
                auto* fieldDecl = member->fieldDeclaration();
                if (!fieldDecl->typeType() || !fieldDecl->variableDeclarators()) continue;
                const string declared = fieldDecl->typeType()->getText();
                int scalarParamIndex = -1;
                for (size_t k = 0; k < typeParameters.size(); k++) {
                    if (declared == typeParameters[k].name) {
                        scalarParamIndex = (int) k;
                        break;
                    }
                }
                if (scalarParamIndex < 0) continue;
                for (auto* vd : fieldDecl->variableDeclarators()->variableDeclarator()) {
                    if (!vd->variableDeclaratorId()) continue;
                    auto it = inst->getProperties().find(
                        vd->variableDeclaratorId()->getText());
                    if (it != inst->getProperties().end() && it->second) {
                        it->second->setOriginTypeParamIndex(scalarParamIndex);
                    }
                }
            }
        }

        // Record templates skip the declaration-time body walk, so the
        // no-abstract-method gate re-runs here per instantiation.
        if (recordDecl) {
            for (auto& kv : inst->getMethods()) {
                if (kv.second && kv.second->isAbstract()) {
                    throw Exception(
                        "record '" + instQName->toCanonical()
                            + "' declares abstract method '"
                            + kv.second->getName()
                            + "' — records have no vtable; every record "
                              "method needs a body",
                        "CAJETA_ERROR_RECORD_ABSTRACT_METHOD");
                }
            }
        }

        // The declaration-time synthesizer seam never ran for this class (its
        // body came through visitClassBody), so member + companion synthesizers
        // run here with type args bound, before generatePrototype lays them out.
        visitor.runMemberSynthesizers(inst);
        visitor.runCompanionSynthesizers(inst);

        {
            const auto protoT0 = std::chrono::steady_clock::now();
            inst->generatePrototype();
            g_protoNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - protoT0).count();
        }
        } catch (...) {
            CajetaModule::setActiveModule(prevActive);
            stack.clear();
            stack.swap(savedStack);
            module->popTypeSubstitution();
            throw;
        }

        CajetaModule::setActiveModule(prevActive);
        stack.clear();
        stack.swap(savedStack);
        module->popTypeSubstitution();

        CajetaModule::getStructureToModule()[instCanonical] = emitOwner;
        return inst;
    }

    // --- Diamond inference (TPL-7) ----------------------------------------

    // Unify one (formal parameter typeType, arg type) pair, adding bindings as
    // type-parameter names match; false on any conflict. Recursive: nested args
    // in `List<T>` / `Box<Pair<A,B>>` walk alongside the arg's typeArguments.
    static bool unifyParam(
        CajetaParser::TypeTypeContext* paramTT,
        CajetaTypePtr argType,
        const std::set<string>& paramNames,
        std::map<string, CajetaTypePtr>& bindings) {
        if (!paramTT || !argType) return false;

        // Primitive and array slots cannot bind a type parameter.
        if (paramTT->primitiveType()) return true;
        if (!paramTT->LBRACK().empty()) return true;

        auto* coi = paramTT->classOrInterfaceType();
        if (!coi) return true;

        const auto& ids = coi->identifier();
        if (ids.size() != 1) return true;
        const string ident = ids[0]->getText();
        auto* targs = coi->typeArguments(0);

        if (paramNames.count(ident)) {
            if (targs) {
                // A type parameter is a type, not a template.
                return false;
            }
            auto existing = bindings.find(ident);
            if (existing == bindings.end()) {
                bindings[ident] = argType;
                return true;
            }
            return existing->second->toCanonical() == argType->toCanonical();
        }

        if (targs) {
            auto argClass = dynamic_pointer_cast<CajetaClass>(argType);
            if (!argClass || !argClass->isInstantiation()) return false;
            auto argTemplate = argClass->getTemplateOrigin();
            if (!argTemplate) return false;
            if (argTemplate->getQName()->getTypeName() != ident) return false;
            auto& argTypeArgs = argClass->getTypeArguments();
            auto innerTArgs = targs->typeArgument();
            if (argTypeArgs.size() != innerTArgs.size()) return false;
            for (size_t i = 0; i < innerTArgs.size(); ++i) {
                if (!innerTArgs[i]->typeType()) return false;
                if (!unifyParam(innerTArgs[i]->typeType(), argTypeArgs[i],
                        paramNames, bindings)) {
                    return false;
                }
            }
            return true;
        }

        // A concrete unparameterized slot binds nothing; call-site
        // compatibility is the caller's own check.
        return true;
    }

    vector<CajetaTypePtr> CajetaClass::inferDiamondArgs(
        const vector<CajetaTypePtr>& argTypes) {
        if (!isTemplate()) {
            throw Exception(
                "diamond inference invoked on non-template type "
                    + qName->toCanonical(),
                "CAJETA_ERROR_TYPE_INFERENCE");
        }

        // The same re-parse as instantiate, for inspection only: no
        // substitution is pushed and no IR is emitted.
        xref::SyntheticSourceScope xrefMask;

        string input = synthesizePreamble(module) + templateSource + "\n";
        antlr4::ANTLRInputStream inputStream(input);
        CajetaLexer lexer(&inputStream);
        antlr4::CommonTokenStream tokens(&lexer);
        tokens.fill();
        CajetaParser parser(&tokens);
        auto* compUnit = dynamic_cast<CajetaParser::CompilationUnitContext*>(
            parseSyntheticCompilationUnit(parser, tokens,
                                          "synthetic:inspect"));

        CajetaParser::ClassBodyContext* declBody = nullptr;
        for (auto* td : compUnit->typeDeclaration()) {
            if (auto* cd = td->classDeclaration()) {
                declBody = cd->classBody();
                break;
            }
            if (auto* rd = td->recordDeclaration()) {
                declBody = rd->classBody();
                break;
            }
        }
        if (!declBody) {
            throw Exception(
                "diamond inference: template snippet missing classDeclaration",
                "CAJETA_ERROR_TYPE_INFERENCE");
        }

        std::set<string> paramNames;
        for (auto& p : typeParameters) paramNames.insert(p.name);

        // A ctor is viable when its arity matches; v1 does no widening/boxing.
        vector<std::map<string, CajetaTypePtr>> viableBindings;

        for (auto* bdCtx : declBody->classBodyDeclaration()) {
            auto* md = bdCtx->memberDeclaration();
            if (!md) continue;
            auto* ctorDecl = md->constructorDeclaration();
            if (!ctorDecl) continue;
            auto* fps = ctorDecl->formalParameters();
            std::vector<CajetaParser::FormalParameterContext*> params;
            if (auto* list = fps->formalParameterList()) {
                params = list->formalParameter();
            }
            if (params.size() != argTypes.size()) continue;

            std::map<string, CajetaTypePtr> bindings;
            bool ok = true;
            for (size_t i = 0; i < params.size(); ++i) {
                if (!unifyParam(params[i]->typeType(), argTypes[i],
                        paramNames, bindings)) {
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;
            viableBindings.push_back(std::move(bindings));
        }

        if (viableBindings.empty()) {
            throw Exception(
                "diamond inference: no constructor of "
                    + qName->toCanonical()
                    + " matches the argument list",
                "CAJETA_ERROR_TYPE_INFERENCE");
        }
        if (viableBindings.size() > 1) {
            throw Exception(
                "diamond inference: multiple constructors of "
                    + qName->toCanonical()
                    + " match the argument list; specify type arguments explicitly",
                "CAJETA_ERROR_TYPE_INFERENCE");
        }

        vector<CajetaTypePtr> resolved;
        for (auto& tp : typeParameters) {
            auto it = viableBindings[0].find(tp.name);
            if (it == viableBindings[0].end()) {
                throw Exception(
                    "diamond inference: type parameter '" + tp.name
                        + "' of " + qName->toCanonical()
                        + " could not be inferred from the constructor args",
                    "CAJETA_ERROR_TYPE_INFERENCE");
            }
            resolved.push_back(it->second);
        }
        return resolved;
    }

}  // namespace cajeta
