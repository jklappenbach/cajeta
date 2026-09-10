// Per-call monomorphization of a method-templated declaration into a concrete Method:
// synthesize a re-parseable snippet, walk it under a substitution map binding each
// T-var, then reparent the extracted Method to the template's own parent class.

#include "Method.h"
#include "../type/CajetaClass.h"
#include "../type/QualifiedName.h"
#include "../asn/ClassBodyDeclaration.h"
#include "cajeta/synth/SynthesizerRegistry.h"
#include "../compile/CajetaModule.h"
#include "../compile/CajetaLlvmVisitor.h"
#include "../compile/Compiler.h"
#include "../error/Exception.h"
#include "CajetaParser.h"
#include "CajetaLexer.h"
#include "cajeta/synth/SourceSynthesisParse.h"

#include "antlr4-runtime/antlr4-runtime.h"

#include <cstdlib>
#include <iostream>
#include "cajeta/xref/XrefIndex.h"

namespace cajeta {

    // `<arg0,arg1,...>` from the arg canonicals: the cache key and symbol suffix.
    static std::string buildMethodArgSuffix(const std::vector<CajetaTypePtr>& args) {
        std::string s = "<";
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) s += ",";
            s += args[i]->getQName()->toCanonical();
        }
        s += ">";
        return s;
    }

    // The package + imports the re-parsed snippet needs to resolve the same names the
    // template body was written against — the shape class-template instantiation uses.
    static std::string synthesizeMethodPreamble(CajetaModulePtr module) {
        std::string out;
        const std::string& pkg = module->getQName()->getPackageName();
        if (!pkg.empty()) {
            out += "package " + pkg + ";\n";
        }
        for (auto& byType : module->getImports()) {
            const std::string& typeName = byType.first;
            for (auto& byPkg : byType.second) {
                const std::string& pkgName = byPkg.first;
                out += "import ";
                if (!pkgName.empty()) {
                    out += pkgName + ".";
                }
                out += typeName + ";\n";
            }
        }
        return out;
    }

    // Monomorphize this template over `args` into a concrete Method, cached per arg
    // list, and record it as an obligation of whichever module's codegen triggered it.
    MethodPtr Method::instantiateMethodTemplate(std::vector<CajetaTypePtr> args) {
        MethodPtr inst = instantiateMethodTemplateInternal(std::move(args));
        // Noted in this WRAPPER, not the internal, so a cached hit is recorded too.
        if (inst) {
            CajetaModule::noteCrossModuleMethodInstantiation(
                CajetaModule::getCurrentCodegenModule(), inst);
        }
        return inst;
    }

    // Build (or cache-hit) the closure-specialized instance F<args>$fn. The post-parse
    // transform lives in the internal below, gated on `spec`.
    MethodPtr Method::instantiateSpecializedClosure(std::vector<CajetaTypePtr> args,
            const std::string& paramName, llvm::Function* fn, CajetaTypePtr fnType,
            llvm::Constant* record) {
        ClosureSpecialization spec{paramName, fn, std::move(fnType), record};
        MethodPtr inst = instantiateMethodTemplateInternal(std::move(args), &spec);
        if (inst) {
            CajetaModule::noteCrossModuleMethodInstantiation(
                CajetaModule::getCurrentCodegenModule(), inst);
        }
        return inst;
    }

    // The shared body: check arity + bounds, invalidate and consult the per-arg cache,
    // choose an emit module, then re-parse the captured source under the substitution
    // and reparent the extracted Method. A non-null `spec` adds the closure transform.
    MethodPtr Method::instantiateMethodTemplateInternal(std::vector<CajetaTypePtr> args,
            const ClosureSpecialization* spec) {
        if (!isMethodTemplate()) {
            throw Exception(
                "instantiateMethodTemplate invoked on non-template method "
                    + buildCanonical(parent, name, parameterList, false),
                "CAJETA_ERROR_METHOD_TEMPLATE_INVALID");
        }

        if (args.size() != methodTypeParameters.size()) {
            throw Exception(
                "method template '" + name + "' expects "
                    + std::to_string(methodTypeParameters.size())
                    + " type argument(s), got " + std::to_string(args.size()),
                "CAJETA_ERROR_METHOD_TEMPLATE_ARITY");
        }

        // Each declared bound must hold for the supplied arg. wildcardStubBody: a
        // BOUNDED parameter given a wildcard sentinel keeps its SIGNATURE but takes a
        // throw-stub body, since `?` elements cannot compile.
        bool wildcardStubBody = false;
        for (size_t i = 0; i < methodTypeParameters.size(); ++i) {
            const auto& param = methodTypeParameters[i];
            // A non-type parameter (`<uint32 N>`) requires a compile-time integer
            // constant and a type parameter must not receive one; neither has bounds.
            bool argIsConst = args[i] && (args[i]->getTypeFlags() & CONSTANT_FLAG);
            if (param.isNonType && !argIsConst) {
                throw Exception(
                    "method template '" + name + "': non-type parameter '"
                        + param.name + "' (" + param.nonTypePrimitive
                        + ") requires an integer-constant argument",
                    "CAJETA_ERROR_TYPE_PARAMETER_KIND");
            }
            if (!param.isNonType && argIsConst) {
                throw Exception(
                    "method template '" + name + "': type parameter '"
                        + param.name + "' cannot take an integer-constant argument",
                    "CAJETA_ERROR_TYPE_PARAMETER_KIND");
            }
            if (param.bounds.empty()) continue;
            // An unsubstituted type variable on a bounded parameter: nothing concrete
            // to test, so defer the bound check and flag it for a stub body.
            if (args[i] && args[i]->isWildcard()) {
                wildcardStubBody = true;
                continue;
            }
            auto argClass = std::dynamic_pointer_cast<CajetaClass>(args[i]);
            for (auto& bound : param.bounds) {
                // A numeric marker bound is satisfied intrinsically by a primitive
                // (boolean excluded) and nominally by a class.
                const std::string& bname = bound->getTypeName();
                if (CajetaClass::isNumericMarkerName(bname)) {
                    if (!CajetaClass::satisfiesNumericMarker(args[i], bname)) {
                        throw Exception(
                            "method template '" + name + "': argument '"
                                + (args[i] && args[i]->getQName()
                                    ? args[i]->getQName()->toCanonical()
                                    : std::string("?"))
                                + "' does not satisfy numeric bound '" + bname
                                + "' on parameter '" + param.name + "'",
                            "CAJETA_ERROR_METHOD_TEMPLATE_BOUND");
                    }
                    continue;
                }
                auto& cmap = CajetaType::getCanonicalMap();
                CajetaTypePtr boundType;
                auto it = cmap.find(bound->toCanonical());
                if (it != cmap.end()) {
                    boundType = it->second;
                } else {
                    auto nit = cmap.find(bound->getTypeName());
                    if (nit != cmap.end()) boundType = nit->second;
                }
                auto boundClass = std::dynamic_pointer_cast<CajetaClass>(boundType);
                if (!boundClass) {
                    throw Exception(
                        "method template '" + name + "': bound '"
                            + bound->getTypeName() + "' on parameter '"
                            + param.name + "' did not resolve to a class",
                        "CAJETA_ERROR_METHOD_TEMPLATE_BOUND");
                }
                if (!argClass || !argClass->isParentOrKind(boundClass)) {
                    throw Exception(
                        "method template '" + name + "': argument '"
                            + args[i]->getQName()->toCanonical()
                            + "' does not satisfy bound '"
                            + bound->getTypeName() + "' on parameter '"
                            + param.name + "'",
                        "CAJETA_ERROR_METHOD_TEMPLATE_BOUND");
                }
            }
        }

        // This cache lives on the persistent stdlib template Method, so a hit from a
        // PRIOR test would return an instantiation whose emit module has been freed.
        uint64_t epoch = CajetaModule::getReuseEpoch();
        if (epoch != methodInstantiationCacheEpoch) {
            // Evict ONLY instantiations whose IR lives in a per-test emit module: one
            // built into the persistent stdlib is module-stable and must stay cached,
            // or the next trigger rebuilds it and defines the same symbol twice.
            for (auto it = methodInstantiationCache.begin();
                    it != methodInstantiationCache.end();) {
                MethodPtr inst = it->second;
                bool stdlibResident = inst
                    && inst->getEmitModule() == inst->getModule();
                if (stdlibResident) {
                    // ...but only while its DEFINITION survives in the stdlib module.
                    // Keeping an erased entry short-circuits the re-emit and the next
                    // session's link misses the symbol, so trust the IR, not the map.
                    llvm::Module* sm = inst->getModule()
                        ? inst->getModule()->getLlvmModule() : nullptr;
                    llvm::Function* fn = sm
                        ? sm->getFunction(inst->getLlvmSymbolName())
                        : nullptr;
                    if (fn && !fn->isDeclaration()) {
                        ++it;
                        continue;
                    }
                }
                if (inst && parent) {
                    // Full unregister from EVERY per-class map: addMethod's
                    // duplicate-static check reads unlabeledMethodMap too.
                    parent->removeMethod(inst);
                }
                it = methodInstantiationCache.erase(it);
            }
            methodInstantiationCacheEpoch = epoch;
        }

        // A closure specialization keys a SEPARATE slot, one per (type-args, bound
        // lambda fn), so it re-parses its own AST and never aliases the base.
        std::string suffix = buildMethodArgSuffix(args);
        if (spec && spec->fn) suffix += "$spec$" + spec->fn->getName().str();
        auto cached = methodInstantiationCache.find(suffix);
        if (cached != methodInstantiationCache.end()) {
            return cached->second;
        }

        // Emit target: a stdlib method template specialized over a USER type emits into
        // the user module so the cached stdlib stays pristine. Chosen from a user-type
        // arg, the receiver class, the codegen frame, or the per-test sink.
        CajetaModulePtr emitOwner = module;
        if (Compiler::getSharedContext()
                && module == CajetaModule::getStdlibModule()) {
            for (auto& arg : args) {
                auto ac = std::dynamic_pointer_cast<CajetaClass>(arg);
                if (ac && ac->getEmitModule() && ac->getEmitModule() != module) {
                    emitOwner = ac->getEmitModule();
                    break;
                }
            }
            if (emitOwner == module && parent && parent->getEmitModule()
                    && parent->getEmitModule() != module) {
                emitOwner = parent->getEmitModule();
            }
            // SESSION policy — the twin of the class-template gate.
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

        // Reuse-cache hazard gate (test-only). With emitOwner != module a NOVEL
        // instantiation's IR lands in a per-test module while the host template
        // persists, leaving pointers that dangle next test — abort before the walk.
        if (Compiler::isReuseHazardArmed() && emitOwner != module) {
            // CAJETA_REUSE_FORCE_EMIT=1 selects the cross-module emit path instead of
            // the fresh fallback. It stays OFF by default: the twin CLASS-template gate
            // in TemplateInstantiator.cpp still needs the same reparenting fix.
            static const bool kForceEmit =
                std::getenv("CAJETA_REUSE_FORCE_EMIT") != nullptr;
            if (!kForceEmit) {
                throw cajeta::ReuseHazardAbort{};
            }
        }

        if (methodSource.empty()) {
            throw Exception(
                "method template '" + name + "' has no captured source; "
                "method-level templates must be parsed via the standard "
                "visitor path before instantiation",
                "CAJETA_ERROR_METHOD_TEMPLATE_INVALID");
        }

        // JSON synthesizer hook: for Json.parse<T> / Json.toBytes<T> the captured
        // throw-body source is replaced by a per-T synthesized body before re-parsing.
        // It stays the failsafe for unrecognized entry points and non-matching overloads.
        std::string effectiveSource = methodSource;
        {
            std::vector<CajetaTypePtr> paramTypes;
            for (auto& fp : parameterList) {
                if (!fp) continue;
                if (fp->getName() == "this") continue;
                CajetaTypePtr pt = fp->getType();
                // Present type-parameter-typed formals under THIS instantiation's
                // bindings: a captured formal bound its bare type name at TEMPLATE-PARSE
                // time, where `T` is no class, so rebind by declared simple name.
                const std::string& declared = fp->getDeclaredTypeParamName();
                for (size_t ti = 0; ti < methodTypeParameters.size()
                        && ti < args.size(); ++ti) {
                    if (methodTypeParameters[ti].isNonType) continue;
                    const std::string& tpName = methodTypeParameters[ti].name;
                    // Primary key: the declaration-time stamp (immutable). Fallback:
                    // the resolved type's simple name still being the T-var name.
                    if (tpName == declared
                            || (declared.empty() && pt && pt->getQName()
                                && pt->getQName()->getTypeName() == tpName)) {
                        pt = args[ti];
                        break;
                    }
                }
                paramTypes.push_back(pt);
            }
            // Dispatch through the source-synthesis registry: one match wins, nullopt
            // keeps the captured throw-body, and two matches are a loud error.
            cajeta::synth::registerBuiltinSynthesizers();
            cajeta::synth::SynthesisContext ctx;
            ctx.parent = parent;
            ctx.methodName = name;
            ctx.typeArgs = args;
            ctx.paramTypes = paramTypes;
            ctx.module = module;
            if (auto body = cajeta::synth::SynthesizerRegistry::instance()
                    .dispatchBody(ctx)) {
                effectiveSource = std::move(*body);
                if (const char* dump = std::getenv("CAJETA_DUMP_IR")) {
                    if (dump[0] == '1') {
                        std::cerr << "[Synthesizer] for " << name << "<"
                                  << (args.empty() || !args[0] || !args[0]->getQName()
                                      ? std::string("?")
                                      : args[0]->getQName()->toCanonical())
                                  << ">:\n" << effectiveSource << "\n";
                    }
                }
            }
        }

        // Bounded-at-wildcard: keep the signature, replace the body with a throw stub.
        // The signature text never contains '{', so the first brace opens the body.
        if (wildcardStubBody) {
            auto brace = effectiveSource.find('{');
            if (brace != std::string::npos) {
                effectiveSource = effectiveSource.substr(0, brace)
                    + "{ throw heap Exception(#(\"\" + \"method template '"
                    + name
                    + "' instantiated at wildcard (generic pre-pass) — "
                    + "never executable\")); }";
            }
        }

        // A throwaway wrapper class, so the visitor's class-body walker can build the
        // Method. A unique name per instantiation keeps specializations from colliding.
        std::string wrapperClassName = "__MethodTemplateWrapper_"
            + name + "_" + std::to_string((size_t) this) + "_"
            + std::to_string(methodInstantiationCache.size());
        std::string input = synthesizeMethodPreamble(module)
            + "public class " + wrapperClassName + " {\n"
            + effectiveSource + "\n"
            + "}\n";

        // Snippet positions refer to the snippet, not to a file: mask the parse AND the
        // walk, or the body's callees are attributed to the triggering call site.
        xref::SyntheticSourceScope xrefMask;

        // Leaking parse: the extracted method's AST holds token pointers the later body
        // codegen dereferences, so the pipeline must outlive this call.
        auto* compUnit = cajeta::synth::parseSynthesizedUnit(input);

        CajetaParser::ClassDeclarationContext* classDecl = nullptr;
        for (auto* td : compUnit->typeDeclaration()) {
            if (auto* cd = td->classDeclaration()) {
                classDecl = cd;
                break;
            }
        }
        if (!classDecl) {
            throw Exception(
                "method template '" + name + "': synthesized snippet does "
                "not parse as a classDeclaration",
                "CAJETA_ERROR_METHOD_TEMPLATE_INVALID");
        }

        // Push substitution: method-level T-vars AND the receiver class's own, since
        // Stream<int32>.fold<int64> needs both bound at body-walk time.
        std::map<std::string, CajetaTypePtr> subst;
        if (parent) {
            const auto& classTypeParams = parent->getTypeParameters();
            const auto& classTypeArgs = parent->getTypeArguments();
            if (classTypeParams.size() == classTypeArgs.size()) {
                for (size_t i = 0; i < classTypeParams.size(); ++i) {
                    subst[classTypeParams[i].name] = classTypeArgs[i];
                }
            }
        }
        for (size_t i = 0; i < methodTypeParameters.size(); ++i) {
            subst[methodTypeParameters[i].name] = args[i];
        }
        module->pushTypeSubstitution(subst);

        auto prevActive = CajetaModule::getActiveModule();
        CajetaModule::setActiveModule(module);

        // Isolate the walk with a fresh structure stack: visitMethodDeclaration takes
        // the parent from its front, and we reparent below.
        auto wrapperQName = QualifiedName::getOrInsert(
            wrapperClassName, module->getQName()->getPackageName());
        auto wrapperClass = std::make_shared<CajetaClass>(
            module, wrapperQName, std::list<QualifiedNamePtr>{});
        auto& stack = module->getStructureStack();
        std::list<CajetaClassPtr> savedStack;
        savedStack.swap(stack);
        stack.push_back(wrapperClass);

        // The wrapper's class body holds exactly one method declaration.
        CajetaLlvmVisitor visitor(module);
        auto bodyAny = visitor.visitClassBody(classDecl->classBody());
        auto classBody = std::any_cast<ClassBodyDeclarationPtr>(bodyAny);

        MethodPtr inst;
        for (auto& decl : classBody->getDeclarations()) {
            if (auto md = std::dynamic_pointer_cast<MethodDeclaration>(decl)) {
                if (md->getMethod() && md->getMethod()->getName() == name) {
                    inst = md->getMethod();
                    break;
                }
            }
        }

        stack.clear();
        stack.swap(savedStack);
        CajetaModule::setActiveModule(prevActive);
        module->popTypeSubstitution();

        if (!inst) {
            throw Exception(
                "method template '" + name + "': re-parse produced no "
                "method declaration",
                "CAJETA_ERROR_METHOD_TEMPLATE_INVALID");
        }

        // The body was re-parsed from a synthetic wrapper, so its token lines are
        // SNIPPET lines: correct them against this template's own declLine. Skipped
        // when a synthesizer replaced the body, which has no file position at all.
        if (effectiveSource == methodSource && declLine > 0 && inst) {
            const int snippetLine = inst->getDeclLine();
            if (snippetLine > 0) inst->setDbgLineDelta(declLine - snippetLine);
        }
        inst->setMethodTypeParameters(methodTypeParameters);
        inst->setMethodTypeArguments(args);
        // Reparent to the template's real parent; the visitor set the wrapper class.
        inst->setParentForInstantiation(parent);
        // Emit into the user module in reuse mode; the body is codegen'd later via the
        // cross-module obligation. The walk above built only the AST, no IR yet.
        if (emitOwner != module) inst->setEmitModule(emitOwner);
        // The bare method name is kept: getMapKey / getLlvmSymbolName append the arg
        // suffix for instantiations, so specializations never collide.

        // Back-pointer so the call-site redirect can request a specialization of this
        // template from an instance handle (`this` is the template Method).
        inst->setTemplateOrigin(this);

        // Closure specialization: drop the bound function-typed parameter, record the
        // binding (codegen injects a BoundClosureField), and tag the symbol. The
        // instance stays out of the class maps; the call-site redirect drives it.
        if (spec) {
            inst->dropParameter(spec->paramName);
            inst->addBoundClosure(spec->paramName, spec->fnType, spec->fn, spec->record);
            inst->setSpecializationTag("$spec$" + spec->fn->getName().str());
        }

        methodInstantiationCache[suffix] = inst;
        return inst;
    }

}  // namespace cajeta
