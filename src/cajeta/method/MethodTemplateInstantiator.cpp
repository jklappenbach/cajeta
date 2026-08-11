//
// Method::instantiateMethodTemplate(args) — per-call monomorphization of a
// method-templated declaration into a concrete Method (docs/specification/
// MethodLevelTemplate.md). Mirrors the class-template TemplateInstantiator
// flow: synthesize a re-parseable snippet from the captured method source +
// the module's package / imports, parse with a fresh ANTLR pipeline, push
// a substitution map binding each method-level T-var to the concrete arg,
// walk the body with the existing visitor, then return the resulting Method.
//
// The synthesized snippet wraps the method source in a placeholder class so
// the existing visitor's class-body walker can build the Method object. The
// wrapper class itself is throwaway — we extract the concrete Method out and
// reparent it to the original template's parent class.
//
// Lives in its own TU so Method.h doesn't drag in the visitor + parser
// machinery.
//

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

    // Build `<arg0,arg1,...>` from method-level arg canonicals. Used as cache
    // key and as the mangled-symbol suffix the LLVM function carries.
    static std::string buildMethodArgSuffix(const std::vector<CajetaTypePtr>& args) {
        std::string s = "<";
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) s += ",";
            s += args[i]->getQName()->toCanonical();
        }
        s += ">";
        return s;
    }

    // Reuse the same preamble shape as class-template instantiation so the
    // re-parsed snippet resolves the same names the template body was
    // written against. (Inlined here rather than shared with
    // TemplateInstantiator's `synthesizePreamble` — that function is
    // file-static there. Duplication is tiny and avoids exposing it.)
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

    MethodPtr Method::instantiateMethodTemplate(std::vector<CajetaTypePtr> args) {
        MethodPtr inst = instantiateMethodTemplateInternal(std::move(args));
        // Incremental compilation (Phase 3): record the instantiation as an
        // obligation of whichever module's codegen triggered us. The capture
        // sits in this wrapper — not the internal body — so it also fires when
        // the internal returns a cached instance (a second call site requesting
        // the same specialization during codegen). noteCrossModuleMethodInstantiation
        // no-ops outside codegen (currentCodegenModule null) and for same-module
        // instantiations. Mirrors the CajetaClass::instantiate choke point.
        if (inst) {
            CajetaModule::noteCrossModuleMethodInstantiation(
                CajetaModule::getCurrentCodegenModule(), inst);
        }
        return inst;
    }

    // cajeta-ir Unit 4a — build (or cache-hit) the closure-specialized instance
    // F<args>$fn. Mirrors instantiateMethodTemplate's choke point; the post-parse
    // transform (drop the bound param, record the binding, tag the symbol) lives
    // in the internal below, gated on `spec`.
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

        // Bounds check — same shape as class-template instantiation. Each
        // declared bound must hold for the supplied arg.
        //
        // wildcardStubBody: set when a BOUNDED parameter receives a wildcard
        // sentinel — the generic pre-pass of a templated body forwarding its
        // own (still-unsubstituted) type variable, e.g. `Tensor.add<E>(...)`
        // inside `class GradTape<E extends Floating>`. The pre-pass needs the
        // instantiation's SIGNATURE for type flow, but the body cannot compile
        // at `?` (numeric kernels fail overload resolution on `?` elements) —
        // and it never needs to: each concrete instantiation re-walks the
        // templated body under the substitution stack and requests its own
        // fully-typed instantiation. The wildcard instantiation gets a
        // throw-stub body below (same shape as the Json failsafe throw-body),
        // so it registers, codegens, and is loud in the impossible case it is
        // ever reached at runtime. Before this path existed, bounded-at-
        // wildcard was a hard instantiation error, so no code depends on such
        // bodies being real.
        bool wildcardStubBody = false;
        for (size_t i = 0; i < methodTypeParameters.size(); ++i) {
            const auto& param = methodTypeParameters[i];
            // Parameter-kind check — mirrors the class-level guard in
            // TemplateInstantiator.cpp. A non-type (integer) parameter
            // (`<uint32 N>`) requires a compile-time integer-constant argument
            // (a CajetaConstantType, tagged CONSTANT_FLAG); a type parameter
            // must NOT receive one. Non-type params carry no bounds, so they
            // fall through the bound check below.
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
            // An unsubstituted type variable (wildcard sentinel) on a bounded
            // parameter: defer the bound check (nothing concrete to test) and
            // flag the instantiation for a stub body — see wildcardStubBody.
            if (args[i] && args[i]->isWildcard()) {
                wildcardStubBody = true;
                continue;
            }
            auto argClass = std::dynamic_pointer_cast<CajetaClass>(args[i]);
            for (auto& bound : param.bounds) {
                // Numeric marker bound (Numeric/Floating/Integral/Complex): a
                // primitive type argument satisfies it intrinsically via the
                // type-flag lattice (boolean excluded), a class nominally —
                // the shared dual-conformance predicate (numeric-bounds-spec.md).
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

        // Reuse path: this cache lives on the persistent stdlib template Method,
        // so a hit from a PRIOR test would hand back an instantiation whose emit
        // module (a user module) has been freed. Self-invalidate when the test
        // generation advanced. No-op in production (epoch never bumps).
        uint64_t epoch = CajetaModule::getReuseEpoch();
        if (epoch != methodInstantiationCacheEpoch) {
            // Also UNREGISTER the stale instantiations from the host class's
            // method map. bringMethodTemplateInstantiationToLife (CajetaClass.cpp)
            // registered each there + emitted its body into the PREVIOUS test's
            // (now-freed) user emit module; left in place, that registration makes
            // the next test's codegen short-circuit ("already registered") and
            // never re-emit the body into ITS module — the call site then
            // references an undefined symbol at JIT link. The host is the
            // template's own parent class (persistent stdlib class), so its method
            // map outlives the per-test modules. No-op in production (epoch never
            // bumps; the cache is empty).
            // Evict ONLY the instantiations whose IR lives in a per-test emit
            // module (emitModule override set) — those pointers dangle once the
            // test's module is freed. A specialization built INTO the
            // persistent stdlib (e.g. Sort::pdqFindRun<int64> instantiated
            // during the stdlib prime) is module-stable: its Function outlives
            // every test, so it must stay cached. Evicting it made the next
            // user-module trigger REBUILD it with emitOwner = the user module,
            // producing a second DEFINITION of the same symbol — the
            // "multiply defined" stdlib-clone merge failure under
            // CAJETA_REUSE_FORCE_EMIT (CollectionLiteralTests.
            // ElementTypeFromTarget).
            for (auto it = methodInstantiationCache.begin();
                    it != methodInstantiationCache.end();) {
                MethodPtr inst = it->second;
                bool stdlibResident = inst
                    && inst->getEmitModule() == inst->getModule();
                if (stdlibResident) {
                    // ...but only while its DEFINITION is still in the
                    // stdlib module. restoreLlvmBaseline keeps an
                    // instantiation a BASELINE stdlib body references
                    // (adopted — Sort::pdqFindRun<int64>) and ERASES one
                    // only a dead session's cells referenced (Sort::
                    // sort<int32>). Keeping an erased entry short-circuits
                    // the re-emit and the next resident session's link
                    // misses the symbol (KernelCellTests.
                    // failedCellLeavesBindingsIntact). Trust the IR.
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
                    // Full unregister from EVERY per-class map (not just
                    // `methods`): addMethod's duplicate-static check reads
                    // unlabeledMethodMap, so a partial erase would re-trip it.
                    parent->removeMethod(inst);
                }
                it = methodInstantiationCache.erase(it);
            }
            methodInstantiationCacheEpoch = epoch;
        }

        // Cache hit? A closure specialization (Unit 4a) keys a SEPARATE cache
        // slot — distinct per (type-args, bound lambda fn) — so it re-parses its
        // own fresh AST and never aliases the base instantiation.
        std::string suffix = buildMethodArgSuffix(args);
        if (spec && spec->fn) suffix += "$spec$" + spec->fn->getName().str();
        auto cached = methodInstantiationCache.find(suffix);
        if (cached != methodInstantiationCache.end()) {
            return cached->second;
        }

        // Emit target for this instantiation (resolution/emit separation,
        // reuse-gated) — mirrors the class-template choke point. A stdlib method
        // template specialized over a USER type emits into the user module so
        // the cached stdlib stays pristine. Chosen from: a user-type method arg's
        // emit module; the receiver class's emit module (e.g. Stream<test.M>);
        // the active codegen frame; the per-test sink. module in production.
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

        // Reuse-cache hazard gate (test-only; see Compiler::setReuseHazardArmed
        // and the twin gate in TemplateInstantiator.cpp). We are past the
        // per-arg cache check, so this is a NOVEL method-template instantiation
        // about to be parsed + emitted. When emitOwner != module its body IR
        // (plus its JSON-synth string constants, spawn trampolines, and any
        // nested instantiations) lands in a per-test user module while the
        // host stdlib template Method persists in the shared context — leaving
        // module-bound llvm pointers on persistent objects that outlive that
        // module and dangle on the next reusing test (observed as a null
        // operand mid-body: BinaryOpExpression::generateCode → getTypeFlagsOf,
        // crashing JsonSynthesizerTests.parseNestedClass). Abort BEFORE the
        // walk emits anything so the harness re-runs this test on a fresh,
        // fully-isolated Compiler. Disarmed in production and on the fresh
        // fallback path; a primed (already-cached) instantiation returned above
        // and never reaches here.
        if (Compiler::isReuseHazardArmed() && emitOwner != module) {
            // The cross-module method-template EMIT path (specialize a stdlib
            // method template over a user type, emit the body into the user
            // module, keep the cached stdlib byte-pristine) is the same path
            // production --emit uses, and is now correct under reuse after the
            // scope-barrier fix in bringMethodTemplateInstantiationToLife
            // (CajetaClass.cpp). CAJETA_REUSE_FORCE_EMIT=1 selects it: the test
            // REUSES (fast) instead of degrading to a fresh fallback.
            //
            // It stays OFF by default — DO NOT flip kForceEmit's default to true
            // yet. A clean full-suite W=24 run with FORCE_EMIT=1 (2026-06-10)
            // validated this *method*-template path (zero crashes, stdlib stays
            // byte-pristine) but surfaced a residual cross-test MISCOMPILE on the
            // CLASS-template-with-interface path that the twin gate in
            // TemplateInstantiator.cpp still guards: under FORCE_EMIT,
            // TemplatedInterfaceV2Tests.templatedImplementerInterfaceDispatch
            // emitted an invalid GEP into test.Holder<int32>'s interface vtable
            // slots (JIT verify: "Invalid indices for GEP pointer type"), failing
            // only in a cross-suite sequence. Forcing emit here is only safe once
            // that class-template gate gets the same emit-module reparenting fix.
            // Until then the conservative fresh fallback is the blessed default.
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

        // Tier-1 JSON synthesizer hook (Phase 4b). For
        // Json.parse<T> / Json.toBytes<T> we replace the captured
        // throw-body source with a per-T synthesized body before
        // re-parsing. The captured throw-body stays as the failsafe
        // for unrecognized entry points; overloads of the same name
        // whose param signatures don't match a recognized entry
        // point also keep their captured source (e.g. the
        // `parse<T>(String)` convenience that delegates to
        // `parse<T>(int8[], int64)`).
        std::string effectiveSource = methodSource;
        {
            std::vector<CajetaTypePtr> paramTypes;
            for (auto& fp : parameterList) {
                if (!fp) continue;
                if (fp->getName() == "this") continue;
                CajetaTypePtr pt = fp->getType();
                // Present type-parameter-typed formals under THIS
                // instantiation's bindings. The captured formal resolved its
                // bare type name at TEMPLATE-PARSE time, where a method type
                // parameter (`T value`) isn't a resolvable class — so it
                // lands parse-order-dependently as null or, worse, as an
                // archive-vouched placeholder for an UNRELATED class that
                // shares the short name (a test's own `test.T` entry class
                // was the observed case: every codec's toBytes<T> matcher
                // saw canonical 'test.T', declined, and the placeholder
                // throw-body shipped — "synthesizer not engaged"). Within
                // the template's scope the name DENOTES the type parameter,
                // so rebind by declared simple name; body synthesizers then
                // dispatch on the concrete canonical.
                const std::string& declared = fp->getDeclaredTypeParamName();
                for (size_t ti = 0; ti < methodTypeParameters.size()
                        && ti < args.size(); ++ti) {
                    if (methodTypeParameters[ti].isNonType) continue;
                    const std::string& tpName = methodTypeParameters[ti].name;
                    // Primary key: the declaration-time stamp (immutable).
                    // Fallback: the resolved type's simple name still being
                    // the T-var name (pre-stamp captures, e.g. a restored
                    // reuse baseline primed by an older binary).
                    if (tpName == declared
                            || (declared.empty() && pt && pt->getQName()
                                && pt->getQName()->getTypeName() == tpName)) {
                        pt = args[ti];
                        break;
                    }
                }
                paramTypes.push_back(pt);
            }
            // Dispatch through the source-synthesis registry (spec §2). The
            // codec if-else chain that used to live here is now registered body
            // synthesizers; dispatchBody returns the single match (or nullopt =
            // failsafe, keeping the captured throw-body). Two matches are a loud
            // error (§1.5 [S2]).
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

        // Bounded-at-wildcard pre-pass instantiation: keep the signature,
        // replace the body with a throw stub (see wildcardStubBody above).
        // The signature text never contains '{' — the first brace opens the
        // body. Applied AFTER synthesizer dispatch so a synthesized body is
        // stubbed the same way (it would fail at `?` identically).
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

        // Synthesize a wrapper class around the method source. The wrapper
        // class is throwaway — we extract the concrete Method out of it and
        // reparent to the original template's parent. Using a unique name
        // per instantiation keeps multiple specializations from collision in
        // the visitor's structure stack / canonical map.
        std::string wrapperClassName = "__MethodTemplateWrapper_"
            + name + "_" + std::to_string((size_t) this) + "_"
            + std::to_string(methodInstantiationCache.size());
        std::string input = synthesizeMethodPreamble(module)
            + "public class " + wrapperClassName + " {\n"
            + effectiveSource + "\n"
            + "}\n";

        // Synthesized snippet: its positions refer to the snippet, not to a file.
        // Mask the parse AND the walk below, or the instantiated body's callees are
        // attributed to the user call site that triggered the instantiation
        // (ide-symbol-index 2.2.8).
        xref::SyntheticSourceScope xrefMask;

        // Leaking parse (shared helper): the extracted method's AST holds token
        // pointers the later body codegen dereferences, so the pipeline must
        // outlive this call — the stack-local parse this replaced was a latent
        // dangle that only survived by not-yet-reused freed memory.
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

        // Push substitution: method-level T-vars AND any class-level T-vars
        // from the receiver's enclosing class. The instance-on-templated-
        // receiver case (e.g. Stream<int32>.fold<int64>) needs both T
        // (class-level) and R (method-level) bound at body-walk time —
        // body references like `R acc = seed;` and `Optional<T> o =
        // this.next();` resolve via the same lookup path.
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

        // Isolate the walk with a fresh structure stack containing only a
        // throwaway placeholder for the wrapper. visitMethodDeclaration sets
        // the method's parent from `structureStack().front()`; we'll
        // reparent below to point at the real template's parent class.
        auto wrapperQName = QualifiedName::getOrInsert(
            wrapperClassName, module->getQName()->getPackageName());
        auto wrapperClass = std::make_shared<CajetaClass>(
            module, wrapperQName, std::list<QualifiedNamePtr>{});
        auto& stack = module->getStructureStack();
        std::list<CajetaClassPtr> savedStack;
        savedStack.swap(stack);
        stack.push_back(wrapperClass);

        // Walk the wrapper's class body. The body contains exactly one
        // method declaration — the templated method, but with T-vars now
        // substituted by the real args (placeholder substitution from the
        // initial parse is replaced by the real one pushed above).
        CajetaLlvmVisitor visitor(module);
        auto bodyAny = visitor.visitClassBody(classDecl->classBody());
        auto classBody = std::any_cast<ClassBodyDeclarationPtr>(bodyAny);

        // Find the concrete Method.
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

        // Cache + return. The instantiation carries methodTypeParameters
        // (so isMethodTemplateInstantiation() returns true) and the
        // concrete methodTypeArguments. Subsequent calls with the same
        // arg list hit the cache.
        // 9.2: the body was re-parsed from a synthetic wrapper, so its token
        // lines are SNIPPET lines. Correct them to true file lines using this
        // template's own declLine vs the snippet's declaration line — exact
        // regardless of preamble size. Skipped when a synthesizer replaced the
        // body (that text has no file position at all).
        if (effectiveSource == methodSource && declLine > 0 && inst) {
            // inst->declLine was set from the SNIPPET token during the
            // re-parse; declLine (this template's) is the true file line.
            const int snippetLine = inst->getDeclLine();
            if (snippetLine > 0) inst->setDbgLineDelta(declLine - snippetLine);
        }
        inst->setMethodTypeParameters(methodTypeParameters);
        inst->setMethodTypeArguments(args);
        // Reparent to the original template's parent so downstream
        // canonical-name building uses the right class name. The visitor
        // set it to the wrapper class.
        inst->setParentForInstantiation(parent);
        // Emit this specialization into the user module in reuse mode (its body
        // is codegen'd later via the cross-module obligation recorded by
        // noteCrossModuleMethodInstantiation; Method::generateCode then swaps to
        // this emit module so the body + its spawn trampolines / string
        // constants land in the user module, not the cached stdlib). The walk
        // above only built the AST; no IR has been emitted yet. No-op in
        // production (emitOwner == module).
        if (emitOwner != module) inst->setEmitModule(emitOwner);
        // Keep the bare method name. Two-layer naming (Method::getMapKey
        // + Method::getLlvmSymbolName) appends the method-arg suffix to
        // the addMethod map keys and the LLVM symbol for instantiations,
        // so two instantiations of a template whose T-vars don't appear
        // in value params (e.g. `static <T> int32 sizeOf()`) coexist
        // without colliding in addMethod's duplicate-static check or in
        // LLVM module symbol space. The lambda-expectedType propagator
        // still works because it skips method-template instantiations
        // entirely (looks up the template, not its specializations).

        // cajeta-ir Unit 4a — closure specialization transform. Drop the bound
        // function-typed parameter from the signature, record the binding (codegen
        // injects a BoundClosureField so calls to it go direct), and tag the symbol
        // so this instance is distinct from the base and from a sibling specialized
        // over a different lambda. The instance is NOT registered in the class
        // method maps; the call-site redirect (Unit 4c) drives its codegen.
        // Back-pointer so the Unit 4c redirect can request a specialization of
        // this template from an instance handle. (`this` is the template Method.)
        inst->setTemplateOrigin(this);

        if (spec) {
            inst->dropParameter(spec->paramName);
            inst->addBoundClosure(spec->paramName, spec->fnType, spec->fn, spec->record);
            inst->setSpecializationTag("$spec$" + spec->fn->getName().str());
        }

        methodInstantiationCache[suffix] = inst;
        return inst;
    }

}  // namespace cajeta
