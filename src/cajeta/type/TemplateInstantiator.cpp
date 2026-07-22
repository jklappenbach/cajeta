//
// CajetaClass::instantiate(args) — materialize a concrete class from a
// template under the supplied arguments. Lives in its own TU so CajetaClass.h
// doesn't have to drag in the visitor + parser machinery.
//
// Flow: synthesize a small re-parseable input from the module's package +
// imports + the captured template source snippet, parse it with a fresh
// ANTLR pipeline, find the classDeclaration ctx, push a type-parameter
// substitution map onto the module, walk the body with the existing visitor
// (which causes CajetaType::fromContext to substitute `T` to the bound arg),
// then run the usual prototype generation on the resulting CajetaClass.
//
// Result is cached in `module->getStructures()` under the canonical-with-args
// name (e.g. `pkg.Box<cajeta.int32>`). Subsequent calls with the same args
// hit the cache.
//
// v1 limitations:
//  - Parameterized supers (`class List<T> extends Container<T>`) parse but
//    the typeArguments on the super clause are silently dropped. TPL-5 will
//    extend qExtended to carry typeArgument lists and resolve through the
//    template instantiation cache.
//  - No diamond-operator inference here — that's TPL-7. This function takes
//    pre-resolved CajetaTypePtr args.
//  - No constraint enforcement — TPL-6.
//

#include "CajetaArray.h"
#include "CajetaClass.h"
#include "QualifiedName.h"
#include "../asn/ClassBodyDeclaration.h"
#include "../compile/CajetaModule.h"
#include "../compile/CajetaLlvmVisitor.h"
#include "../compile/Compiler.h"   // Compiler::getSharedContext() — reuse-mode gate
#include "../error/Exception.h"
#include "cajeta/xref/XrefIndex.h"
#include "CajetaParser.h"
#include "CajetaLexer.h"

#include "antlr4-runtime/antlr4-runtime.h"

#include <cassert>

namespace cajeta {

    // Build `<arg0,arg1,...>` from the type arguments using each arg's
    // canonical name. Used both for the instantiation's class name suffix
    // and for the structure-map cache key.
    static string buildArgSuffix(const vector<CajetaTypePtr>& args) {
        string s = "<";
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) s += ",";
            s += args[i]->getQName()->toCanonical();
        }
        s += ">";
        return s;
    }

    // Reconstruct a `package X.Y; import A.B; ...` preamble from what the
    // module captured during the original parse pass. Required so the
    // re-parsed snippet resolves the same names the template body was
    // written against.
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

    CajetaClassPtr CajetaClass::instantiate(vector<CajetaTypePtr> args) {
        CajetaClassPtr result = instantiateInternal(std::move(args));
        // Incremental compilation (Phase 2/3): if this produced a genuine
        // instantiation (a different object than the template `this` — not a
        // non-template passthrough or placeholder short-circuit) and it's
        // owned by another module, record it as an obligation of whatever
        // module's codegen triggered us. noteCrossModuleInstantiation no-ops
        // outside codegen (currentCodegenModule null) and for same-module
        // instantiations. This is the single choke point through which every
        // class-template instantiation flows. (docs/IncrementalCompilation.md.)
        if (result && result.get() != this) {
            CajetaModule::noteCrossModuleInstantiation(
                CajetaModule::getCurrentCodegenModule(), result);
        }
        return result;
    }

    CajetaClassPtr CajetaClass::instantiateInternal(vector<CajetaTypePtr> args) {
        // Non-templates pass through unchanged. Lets callers do
        // `cls->instantiate(args)` without guarding on isTemplate themselves.
        if (!isTemplate()) {
            return static_pointer_cast<CajetaClass>(shared_from_this());
        }

        // Default type arguments — fill omitted TRAILING parameters from their
        // declared defaults (`class Foo<T = float32>` referenced as `Foo`,
        // `Foo<>`, or with fewer args than params). Done here, BEFORE the cache
        // key and arity check below, so `Foo` and `Foo<float32>` fill to the
        // same arg list → the same canonical name and the same cached
        // instantiation (one type identity, not two). A param whose default is
        // empty stops the fill, leaving the arity check to reject the gap — so a
        // non-defaulted template referenced bare still errors, and a default may
        // only follow defaults. The default text resolves against the global
        // canonical map (primitives like `float32`, or a registered class).
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

        // Placeholder-arg short-circuit. Two flavors of placeholder
        // reach here:
        //
        //   (a) T-var placeholder — created by the method-template
        //       visitor when pushing T-vars onto the substitution
        //       stack. The qName has an EMPTY package; the type
        //       name is the bare param letter ("T", "R"). Example:
        //         public final <R> #Stream<R> map((T) -> R fn)
        //       Return type `Stream<R>` is parsed at declaration
        //       time with R bound to a placeholder. Building a real
        //       `Stream<placeholder-R>` would pollute the structure
        //       cache. Short-circuit — the real instantiation
        //       happens when the method is later instantiated with
        //       a concrete R (MethodTemplateInstantiator).
        //
        //   (b) Forward-reference placeholder — created by
        //       CajetaType::fromContext's miss-path when a user
        //       class is referenced before its declaration is
        //       body-walked. The qName carries a real package
        //       (e.g. `test.DemoClass`). The placeholder will be
        //       FILLED IN PLACE when the class's visitClassDeclaration
        //       eventually runs — same shared_ptr, just populated.
        //       So we can proceed with instantiation now; the
        //       resulting instantiation's references to the
        //       placeholder shared_ptr auto-upgrade as the
        //       placeholder fills.
        //
        // The bug fixed here: pre-fix we short-circuited BOTH
        // flavors, so `ArrayList<DemoClass>` in a file parsed
        // before DemoClass's declaration silently degraded to the
        // bare `ArrayList` template — `list.add()`/`list.count()`
        // dispatched against the template's unbound `T` methods,
        // `sizeCount` never incremented, and `count()` returned 0.
        for (auto& arg : args) {
            if (auto cls = dynamic_pointer_cast<CajetaClass>(arg)) {
                if (cls->isPlaceholder()) {
                    bool isTVar = !cls->getQName()
                        || cls->getQName()->getPackageName().empty();
                    // Templated-INTERFACE instantiation continues to
                    // short-circuit on any placeholder arg. The
                    // interface vtable verification is signature-
                    // strict; building a real `Encoder<placeholder-M>`
                    // and running the conformance check against an
                    // implementer that uses `static` methods (a
                    // @Encoding typeclass pattern) flags the
                    // static-vs-instance mismatch — pre-fix the
                    // short-circuit hid that. Until the verification
                    // learns to accept static methods as substitutes,
                    // we preserve the pre-fix behavior for
                    // interfaces.
                    if (isTVar || interfaceFlag) {
                        return static_pointer_cast<CajetaClass>(
                            shared_from_this());
                    }
                    // Forward-ref placeholder on a class template —
                    // fall through to the normal instantiation path.
                    // Body walk uses CajetaClass::getLlvmType() which
                    // returns ptr for unfilled placeholders, so
                    // layouts compose correctly. When the placeholder
                    // is later filled in, the instantiation's
                    // references (held by shared_ptr in field types,
                    // method signatures) see the populated class
                    // automatically — same shared_ptr identity.
                }
                // Also short-circuit when the arg is itself a bare
                // (uninstantiated) template — the trail left by a
                // transitive short-circuit one level deeper.
                // `HashMapEntryStream<K,V> implements Splittable<Pair<K,V>>`
                // (K,V placeholder) → `Pair.instantiate([K_ph, V_ph])`
                // returns bare Pair via the check above → without this
                // second guard, `Splittable.instantiate([bare-Pair])`
                // proceeds to build a real `Splittable<raw-Pair>` →
                // `Stream<raw-Pair>` cascade whose ::reduce / ::fold<R>
                // codegen segfaults on R=raw-Pair (no LLVM type).
                if (cls->isTemplate() && !cls->getTypeParameters().empty()) {
                    return static_pointer_cast<CajetaClass>(
                        shared_from_this());
                }
            }
        }

        // Step 5a — template wildcards. The Step 1/2 "erased proxy"
        // short-circuit was removed: wildcard args now flow through
        // the normal instantiation path so methods, fields, vtable,
        // and drop walk are all populated by re-parsing the body
        // with T → wildcardSentinel substitution active. The
        // wildcard sentinel is ptr-shaped (STRUCT_FLAG only — see
        // CajetaType::init) so any `T value` field, `(T) -> R fn`
        // parameter, or `Stream<T>` reference inside the body lowers
        // as a pointer slot or a re-entrant Stream<?> instantiation.
        // The cache below is keyed on canonical-with-args so
        // `Box<?>` and `Box<int32>` get distinct entries — the same
        // cache-bucket invariants Step 3 verified still hold.

        if (args.size() != typeParameters.size()) {
            throw Exception(
                "template " + qName->toCanonical() + " expects "
                    + std::to_string(typeParameters.size())
                    + " type argument(s), got " + std::to_string(args.size()),
                "CAJETA_ERROR_TYPE_PARAMETER_ARITY");
        }

        // Constraint enforcement (TPL-6). For each `<T extends Bound>` pair,
        // verify the supplied argument satisfies the bound by walking the
        // argument's supertype chain. Multiple bounds (`<T extends A & B>`)
        // must all be satisfied. The check is purely compile-time — by the
        // time IR is emitted only well-typed instantiations exist, so the
        // runtime never re-validates this.
        //
        // Primitives never satisfy class/interface bounds and fall through
        // to the rejection path — they're not CajetaClass instances and
        // can't carry a parent chain. That's the intended behavior: bounds
        // demand a class/interface relationship.
        for (size_t i = 0; i < typeParameters.size(); ++i) {
            const auto& param = typeParameters[i];
            // Parameter-kind check. A non-type (integer) parameter
            // (`<..., uint32 N>`) requires a compile-time integer-constant
            // argument (a CajetaConstantType, tagged CONSTANT_FLAG); a type
            // parameter must NOT receive one. This is what lets
            // `CooperativeMatrix<T, uint32 Rows, ...>` carry its shape in the
            // type. Non-type params have empty bounds, so they fall through the
            // TPL-6 bound check below — this is their only constraint.
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
            // Wildcard args satisfy any bound by construction —
            // the unbounded `?` stands for some unknown T-that-fits;
            // bounded forms (`? extends Bound`) are deferred to Step 6
            // and won't reach here (the parser rejects them at
            // CajetaType.cpp:454 under flag-on).
            if (args[i] && args[i]->isWildcard()) continue;
            auto argClass = dynamic_pointer_cast<CajetaClass>(args[i]);
            for (auto& bound : param.bounds) {
                // Numerics for Bounded Templates. `T extends Numeric`,
                // `T extends Integral`, `T extends Floating` are three
                // built-in pseudo-bounds that admit primitive numeric
                // type arguments — the bound isn't a class on the
                // canonical chain, it's a category check against the
                // primitive flag bits set in CajetaType.h. Boolean is
                // explicitly excluded from Numeric/Integral even though
                // it carries INT_FLAG | NUMBER_FLAG (its flags are for
                // legacy zero/one storage, not arithmetic).
                const std::string& bname = bound->getTypeName();
                // Numerics for Bounded Templates. `T extends Numeric/Integral/
                // Floating/Complex` are built-in marker bounds with **dual**
                // conformance: a primitive satisfies intrinsically (the type-flag
                // lattice) and a class satisfies nominally (`implements`). Both go
                // through the single predicate so the class-template site agrees
                // with the method-template + wildcard sites.
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
                    continue;  // bound satisfied (intrinsic or nominal)
                }
                // Resolve the bound: try the bound's full canonical first,
                // then fall back to its short name (matches how `extends`
                // names are looked up). canonicalMap holds the template's
                // entry under both keys.
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
                // argClass->isParentOrKind(boundClass) returns true when
                // boundClass is argClass itself or an ancestor.
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

        // Cache key: full canonical name with args, e.g. `pkg.Box<cajeta.int32>`.
        // element-ownership §8.1 (plan 2.1.4) — the value-semantics gate. `#`
        // demands a separable ownership story from the argument: a primitive
        // carries none (§8.1.1); a value type owns heap payload only when
        // shared-capable — Utf8/Slice or a transitive embedder (§8.1.2).
        // Checked after default-fill normalization and before the cache key so
        // both source-level threading (`Box<#int32>` via fromContext /
        // NewExpression) and direct instantiate() callers hit it. Wildcards
        // and still-unfilled forward-ref placeholders skip — their shape isn't
        // known yet; arrays own their elements and pass.
        // title-tracking §8.1 (plan 7.2.1) — the declaration-`#`
        // owning-required gate and the `#`-type-argument agreement/confinement
        // gates (value-type/primitive `#`, BORROW_MODE_OWNED) were retired:
        // type-position `#` errors at parse (TYPE_TRANSFER_RETIRED).
        string suffix = buildArgSuffix(args);
        string instCanonical = qName->toCanonical() + suffix;

        // Emit target (resolution/emit separation, reuse-gated). The
        // instantiation is RESOLVED against `module` (the stdlib template
        // module — its imports/substitution), but its IR is EMITTED into
        // `emitOwner`. In production emitOwner == module (bit-for-bit
        // unchanged). In the stdlib test-reuse path a stdlib template
        // specialized over a USER type emits into that user type's module, so
        // the cached stdlib stays pristine across tests. The owner is chosen at
        // CREATION time (instantiation IR emits during the body walk below, so
        // the emit module must be known before then) by, in order: the first
        // user-type argument's emit module (principled — the instantiation can't
        // outlive its type arg; nested user-type args resolve transitively); the
        // active codegen frame; the harness's per-test fallback sink. Methods
        // adopt `inst`'s emit module at construction (Method ctor), so IR
        // emitted mid-walk already targets the user module — no after-the-fact
        // reparent. Pure-stdlib instantiations (`Stream<String>`) keep stdlib
        // args → emitOwner stays the stdlib module (primed once).
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
        auto cached = structures.find(instCanonical);
        if (cached != structures.end()) {
            return cached->second;
        }
        // Process-wide check via the structureToModule registry. Templated
        // classes referenced cross-module produce one instantiation, owned
        // by whichever module triggered the first build (commonly the user
        // module that named `T<args>` before the template's real
        // declaration parsed — placeholder-template path in
        // CajetaType::fromContext). Without this lookup, a later parse in
        // the template's actual module would re-build the same `T<args>`,
        // producing duplicate `#RttiGlobal` / `#VTable` definitions across
        // .o files and a linker error.
        {
            auto& structToMod = CajetaModule::getStructureToModule();
            auto stIt = structToMod.find(instCanonical);
            if (stIt != structToMod.end() && stIt->second) {
                auto& owningStructures = stIt->second->getStructures();
                auto cachedGlobal = owningStructures.find(instCanonical);
                if (cachedGlobal != owningStructures.end()) {
                    return cachedGlobal->second;
                }
            }
        }

        // Reuse-cache hazard gate (test-only; see Compiler::setReuseHazardArmed).
        // We are past every cache lookup, so this is a NOVEL instantiation about
        // to be built. When emitOwner != module its IR emits into a per-test user
        // module, leaving module-bound llvm pointers cached on persistent objects
        // that outlive that module → cross-module references on the next reusing
        // test. Abort BEFORE the build emits anything so the harness re-runs this
        // test on a fresh, fully-isolated Compiler. Disarmed in production and on
        // the fresh fallback path; a primed (already-cached) instantiation
        // returned above and never reaches here.
        //
        // Unlike the method-template gate (MethodTemplateInstantiator.cpp), this
        // class-template gate has NO CAJETA_REUSE_FORCE_EMIT escape hatch: the
        // cross-module CLASS-template emit path is NOT yet correct under reuse.
        // The 2026-06-10 W=24 FORCE_EMIT run caught it producing an invalid GEP
        // into test.Holder<int32>'s interface vtable slots
        // (TemplatedInterfaceV2Tests.templatedImplementerInterfaceDispatch). This
        // gate is what keeps that miscompile from shipping — it must stay an
        // unconditional fresh fallback until the struct/vtable layout pointers get
        // the same emit-module reparenting the method path received.
        if (Compiler::isReuseHazardArmed() && emitOwner != module) {
            throw cajeta::ReuseHazardAbort{};
        }

        // Templated-interface instantiation. We re-parse the captured
        // interfaceDeclaration source under the type-parameter
        // substitution and build a real instantiated interface — same
        // shape as the class path below but targeting an
        // interfaceDeclaration ctx and walking the interface-body
        // method signatures inline (visitInterfaceDeclaration's body
        // walk doesn't have a standalone visitor entry point, so we
        // duplicate it here with substitution active).
        if (interfaceFlag) {
            if (templateSource.empty()) {
                // Pre-fix safety net for stdlib interfaces whose
                // templateSource didn't get captured (e.g. interfaces
                // declared before the visitor change). Hand back the
                // template — preserves the pre-fix behavior for
                // @Encoding's verification-only path. Once stdlib is
                // re-parsed under the new visitor this branch is dead.
                return static_pointer_cast<CajetaClass>(shared_from_this());
            }

            // The snippet's positions refer to the snippet, not to any file — mask
            // the parse and the walk so the instantiated body's callees are not
            // attributed to the user call site that triggered it (2.2.8).
            xref::SyntheticSourceScope xrefMask;

            string ifInput = synthesizePreamble(module) + templateSource + "\n";
            antlr4::ANTLRInputStream ifStream(ifInput);
            CajetaLexer ifLexer(&ifStream);
            antlr4::CommonTokenStream ifTokens(&ifLexer);
            ifTokens.fill();
            CajetaParser ifParser(&ifTokens);
            auto* ifUnit = ifParser.compilationUnit();
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

            // Interface grammar: `(EXTENDS typeList)?` — interfaces
            // don't have an `implements` clause, so we only walk the
            // single optional typeList. Substitution is already active
            // (pushed above), so `extends Reader<T>` instantiates to
            // `Reader<int32>` when this template is being instantiated
            // for T=int32.
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
            // Resolution module = `module` (stdlib template); emit target set
            // separately below so IR lands in the user module in reuse mode.
            auto ifInst = make_shared<CajetaClass>(
                module, ifInstQName, ifExtended, ifImplemented);
            ifInst->setIsInterface(true);
            // The instantiation happens at the use site, but the code lives in
            // the template's file — that is what its frames must name.
            ifInst->setDeclaringFile(getDeclaringFile());
            if (emitOwner != module) ifInst->setEmitModule(emitOwner);
            ifInst->setTypeParameters(typeParameters);
            ifInst->setTypeArguments(args);
            ifInst->setTemplateOrigin(
                static_pointer_cast<CajetaClass>(shared_from_this()));

            // Cache BEFORE walking — same self-reference rule as the
            // class path. Registered under emitOwner (the module whose codegen
            // worklist emits it and that owns its definition); set
            // structureToModule too so a self-reference resolves when
            // emitOwner != module.
            emitOwner->getStructures()[instCanonical] = ifInst;
            CajetaModule::getStructureToModule()[instCanonical] = emitOwner;

            // Inline interface-body walk: build abstract methods with
            // formal-parameter / return types resolved through the
            // active substitution stack so T → arg.
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
                    // `#T foo()` — return transfers ownership. The normal
                    // visitMethodDeclaration path carries this from the `#`
                    // (REFERENCE) on typeTypeOrVoid; this inline interface-
                    // instantiation walk must do the same or callers of the
                    // instantiated interface method (`Encoder<int32>.encode`)
                    // misclassify the owned `#int8[]` return as a borrow and
                    // never free it (leak). See MemoryModel.md.
                    if (common->typeTypeOrVoid()
                            && common->typeTypeOrVoid()->REFERENCE() != nullptr) {
                        method->setReturnsOwnership(true);
                    }
                    ifBody->getDeclarations().push_back(
                        make_shared<MethodDeclaration>(method, common->getStart()));
                }
            }
            ifInst->setClassBody(ifBody);

            // Emit target propagated to methods at construction (Method ctor
            // reads parent->getEmitModule(), set above before this walk), so no
            // after-the-fact reparent is needed. Interface methods are abstract
            // (no llvm function) anyway.
            ifInst->generatePrototype();

            CajetaModule::setActiveModule(prevActive);
            module->popTypeSubstitution();

            CajetaModule::getStructureToModule()[instCanonical] = emitOwner;
            return ifInst;
        }

        // Re-parse the captured snippet. ANTLR contexts are tied to their
        // parser's lifetime so we don't retain parse trees across compilation
        // phases — we re-parse on demand. The result of this expensive work
        // is cached above; re-parsing only runs once per unique arg list.
        xref::SyntheticSourceScope xrefMask;   // synthesized snippet — see 2.2.8

        string input = synthesizePreamble(module) + templateSource + "\n";

        antlr4::ANTLRInputStream inputStream(input);
        CajetaLexer lexer(&inputStream);
        antlr4::CommonTokenStream tokens(&lexer);
        tokens.fill();
        CajetaParser parser(&tokens);
        auto* compUnit = parser.compilationUnit();

        // The synthesized input wraps one class (or record) declaration. Find
        // it; bail loudly if the snippet structure is somehow wrong (would
        // mean the capture in the visitor produced bad text).
        CajetaParser::ClassDeclarationContext* classDecl = nullptr;
        CajetaParser::RecordDeclarationContext* recordDecl = nullptr;
        for (auto* td : compUnit->typeDeclaration()) {
            if ((classDecl = td->classDeclaration())) break;
            if ((recordDecl = td->recordDeclaration())) break;
        }
        if (!classDecl && !recordDecl) {
            throw "template snippet does not contain a classDeclaration";
        }

        // Build the instantiation's qName: same package as the template,
        // simple name carries the arg suffix. Canonical comes out to
        // `pkg.Box<cajeta.int32>` — readable, distinct, and acceptable as
        // an LLVM struct name.
        string instName = qName->getTypeName() + suffix;
        QualifiedNamePtr instQName = QualifiedName::getOrInsert(
            instName, qName->getPackageName());

        // Build the substitution map (parameter name → arg). Push it before
        // resolving the extends clause so any parameterized super
        // (`extends Container<T>`) gets its typeArguments substituted and
        // routed through instantiate. Without the substitution active here,
        // `T` in the super's typeArguments would fail to resolve and
        // parameterized inheritance would silently drop the args.
        map<string, CajetaTypePtr> subst;
        for (size_t i = 0; i < typeParameters.size(); ++i) {
            subst[typeParameters[i].name] = args[i];
        }
        module->pushTypeSubstitution(subst);
        // Active-module needs to be set here too — fromContext on the super's
        // typeArguments consults activeModule for the substitution stack.
        auto prevActiveForSupers = CajetaModule::getActiveModule();
        CajetaModule::setActiveModule(module);

        // Resolve the extends/implements clauses. The grammar produces
        // typeLists in source order under `classDeclaration`. Match
        // each to its preceding keyword (EXTENDS / IMPLEMENTS / PERMITS)
        // by token index — same logic the top-level visitor uses. For
        // each typeType we call fromContext, which knows how to
        // instantiate parameterized supers/interfaces with the current
        // substitution active.
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
                    // Capture the qName of the (possibly instantiated)
                    // interface plus its resolved type args. Mirrors
                    // the top-level visitor's qImplementedTypeArgs path
                    // so `Holder<T>` in the template captures the
                    // substituted args (e.g. `{int32}` when Box<T> is
                    // being instantiated to Box<int32>).
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
                                // Substitute the template type parameter
                                // (e.g. T) with the concrete arg if it
                                // matches; otherwise keep the identifier.
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

        // Auto-extend Object: a template with no explicit `extends` clause
        // (`class Box<T> { ... }`) implicitly inherits cajeta.lang.Object, just
        // like a regular class. The visitor injects this for ordinary class
        // declarations (CajetaLlvmVisitor.h), but instantiations are built here
        // straight from the parse tree's extends clause, so they bypass that
        // injection. Without it an instantiation has zero parents and is not
        // recognized as `<: Object` — so passing one to an `Object` parameter
        // (e.g. `Class.of(box)`) fails method resolution and silently compiles
        // to null. Mirror the visitor's rule. Skip Object itself (self-cycle).
        // Runs before make_shared so the prepared parent list reaches the ctor.
        bool isObjectItself = instQName
            && instQName->getTypeName() == "Object"
            && instQName->getPackageName() == "cajeta.lang";
        if (instExtended.empty() && !isObjectItself) {
            instExtended.push_back(
                QualifiedName::getOrInsert("Object", "cajeta.lang"));
        }

        // Resolution module = `module` (stdlib template — its imports/
        // substitution drive the body walk); emit target set separately so the
        // class's own IR + its methods' IR (methods adopt this at construction)
        // land in the user module in reuse mode. Set emit BEFORE the walk: the
        // body walk emits method IR (prototype-on-reference), so the emit target
        // must already be in place.
        auto inst = make_shared<CajetaClass>(module, instQName, instExtended, instImplemented);
        // The instantiation happens at the use site, but the code lives in the
        // template's file — that is what its frames must name.
        inst->setDeclaringFile(getDeclaringFile());
        // 9.2: this class body was re-parsed from a synthetic snippet, so every
        // method's token lines are SNIPPET lines (they merely look plausible —
        // the preamble puts them in the file's range; live symptom: F7 into
        // ArrayList::add landed 10 lines off, in the doc comment). The whole
        // snippet shifts uniformly, so one delta — the template's true class
        // declLine minus the snippet's — corrects every method in it.
        if (declLine > 0 && classDecl && classDecl->getStart()) {
            const int snippetClassLine = (int) classDecl->getStart()->getLine();
            if (snippetClassLine > 0) {
                // Stored on the CLASS: methods do not exist yet here (the
                // body walk runs later), and the shift is uniform anyway.
                inst->setDbgLineDelta(declLine - snippetClassLine);
            }
        }
        if (recordDecl) inst->setRecordType(true);
        if (emitOwner != module) inst->setEmitModule(emitOwner);
        // Carry the template's class-level annotations onto the instantiation —
        // they describe the class shape, which the instantiation shares (e.g.
        // @ValueType Entry<K,V> -> Entry<int32,int32> must also be a by-value POD,
        // so generatePrototype below sets VALUE_TYPE_FLAG and the entry stores
        // inline in arrays instead of boxing). The instantiation body is walked
        // via visitClassBody, which never re-runs the visitClassDeclaration
        // annotation handling, so this transfer is the only path.
        for (auto& ann : this->getAnnotationInstances()) {
            inst->addAnnotationInstance(ann);
        }
        inst->setQImplementedTypeArgs(std::move(instImplementedTypeArgs));
        inst->setTypeParameters(typeParameters);   // retained for debugging / introspection
        inst->setTypeArguments(args);
        // Remember which template produced this instantiation. Used by
        // inferDiamondArgs (TPL-N3) to recognize that `List<int32>` is "a
        // List" when unifying against a `List<T>` parameter declaration.
        inst->setTemplateOrigin(static_pointer_cast<CajetaClass>(shared_from_this()));
        // Carry the template's class modifiers (final / abstract / access) onto
        // the instantiation. Without this, `final class ArrayList<T>` produces a
        // NON-final `ArrayList<int32>`, so every `add()` on it stays vtable-
        // dispatched and can't be devirtualized/inlined. The modifiers are a
        // property of the class shape, not the type arguments, so they copy
        // verbatim.
        inst->getModifiers() = this->getModifiers();

        // Cache BEFORE we walk the body. Lets self-referential templates
        // like `class List<T> { List<T> next; }` resolve their own type
        // during the walk — the second reference finds the partially-built
        // instantiation in the cache instead of recursing forever. Registered
        // under `emitOwner` (== module in production); a self-reference during
        // the walk then resolves via the structureToModule branch above (the
        // `module->getStructures()` check misses when emitOwner != module, so
        // structureToModule must be set here too, before the walk).
        emitOwner->getStructures()[instCanonical] = inst;
        CajetaModule::getStructureToModule()[instCanonical] = emitOwner;

        // Isolate the instantiation walk with a clean structure stack
        // containing only `inst`. The visitor's `visitMethodDeclaration`
        // reads `pModule->getStructureStack().front()` to set the method's
        // parent class — if instantiation is triggered while another class
        // is being walked (e.g. D::run mentions `Box<int32> b`), `.front()`
        // would return that outer class and Box's methods would erroneously
        // become D's methods.
        auto& stack = module->getStructureStack();
        list<CajetaClassPtr> savedStack;
        savedStack.swap(stack);
        stack.push_back(inst);

        // Make this module the active one during the walk so deeper helpers
        // that didn't thread `module` through (parse-time Expression /
        // CajetaType construction) can find it and consult its substitution
        // stack. Save+restore around the walk so nested instantiations don't
        // step on each other's active-module state.
        auto prevActive = CajetaModule::getActiveModule();
        CajetaModule::setActiveModule(module);

        // Walk the body with a fresh visitor sharing this module. visitClassBody
        // returns a ClassBodyDeclarationPtr we hand to inst->setClassBody, then
        // generatePrototype lowers the class to LLVM types + functions exactly
        // as it would for a non-templated class.
        //
        // The walk / synthesis / generatePrototype can throw (e.g.
        // CAJETA_ERROR_VTABLE_SLOT_UNRESOLVED in an incremental build). Guard so
        // the substitution frame, active-module, and structure stack are
        // restored on a throw — otherwise a caught-and-continued compile runs
        // with a corrupted module state.
        try {
        CajetaLlvmVisitor visitor(module);
        auto bodyAny = visitor.visitClassBody(
            classDecl ? classDecl->classBody() : recordDecl->classBody());
        inst->setClassBody(std::any_cast<ClassBodyDeclarationPtr>(bodyAny));

        // Field→type-param linkage: the walk above resolved every field
        // type through the substitution stack, so a bare `P` field no longer
        // knows it CAME FROM a type parameter. Record the index on the
        // instantiated property so the drop walk can pick the bit-guarded
        // T-origin branch (emitDropBodyInline).
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

        // Instantiation-time member synthesis (source-synthesis facility,
        // spec §1.5 member/instantiation-time cell). The declaration-time seam
        // in visitClassDeclaration never ran for this concrete class — the
        // instantiation body is walked via visitClassBody above — so run the
        // registered member synthesizers here, with type args now bound. This
        // is where `Table<Tick>` reflects Tick's fields and injects one typed
        // column accessor per field (Unit 5). The substitution frame is still
        // pushed (popped below), and it runs BEFORE generatePrototype so the
        // injected members are laid out and lowered like any other. The
        // instantiation cache above makes this fire once per monomorphization,
        // which is the memoization the accessor set needs (spec §8.3).
        visitor.runMemberSynthesizers(inst);

        // Emit target was set on `inst` before the walk and propagated to each
        // method at construction (Method ctor reads parent->getEmitModule()), so
        // any IR the walk emitted already targets the emit module — no
        // after-the-fact reparent. generatePrototype emits the class's own
        // vtable/RTTI/struct into the emit module via its own swap.
        inst->generatePrototype();
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
    //
    // Walk the template's captured snippet, find constructor declarations,
    // and unify each ctor's parameter types against the supplied argument
    // types. v1 is flat: we look only at the syntactic identifier of each
    // formal parameter's typeType, comparing it directly against type-
    // parameter names. Nested template arguments in parameter types
    // (`Box<List<T>>(...)`) aren't analyzed.
    //
    // Returns the resolved type arguments in declaration order. Throws on
    // ambiguous overload, conflicting bindings, or any type parameter left
    // unbound.

    // Unify a single (formal parameter typeType, arg CajetaType) pair.
    // Adds bindings to `bindings` as type-parameter names get matched. Returns
    // false on any conflict (same T bound to two different args, parameterized
    // name with mismatched outer template, etc.). Recursive: nested template
    // arguments in formal parameter types (`List<T>`, `Box<Pair<A, B>>`) are
    // walked alongside the arg's typeArguments structure.
    static bool unifyParam(
        CajetaParser::TypeTypeContext* paramTT,
        CajetaTypePtr argType,
        const std::set<string>& paramNames,
        std::map<string, CajetaTypePtr>& bindings) {
        if (!paramTT || !argType) return false;

        // Primitive or array formal slot: v1 doesn't do strict type-checks
        // here, but they also can't contribute to template-parameter bindings.
        if (paramTT->primitiveType()) return true;
        if (!paramTT->LBRACK().empty()) return true;

        auto* coi = paramTT->classOrInterfaceType();
        if (!coi) return true;

        const auto& ids = coi->identifier();
        if (ids.size() != 1) return true;       // qualified name — v1 skip
        const string ident = ids[0]->getText();
        auto* targs = coi->typeArguments(0);

        // Pure type-parameter slot (e.g. `T` formal).
        if (paramNames.count(ident)) {
            if (targs) {
                // `T<...>` doesn't make sense — T is a type, not a template.
                return false;
            }
            auto existing = bindings.find(ident);
            if (existing == bindings.end()) {
                bindings[ident] = argType;
                return true;
            }
            return existing->second->toCanonical() == argType->toCanonical();
        }

        // Concrete named slot (e.g. `List<T>` or `Box`). When parameterized,
        // argType must be an instantiation whose templateOrigin's short name
        // matches the formal, and inner args must unify pairwise.
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

        // Plain concrete name (no type args): v1 skips strict checks. Caller
        // is responsible for ensuring the arg is compatible at the call site
        // — diamond inference is just about binding the T's.
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

        // Re-parse the captured snippet to walk constructor declarations.
        // Same machinery as instantiate, just used for inspection — we
        // don't push substitutions or emit IR here.
        xref::SyntheticSourceScope xrefMask;   // synthesized snippet — see 2.2.8

        string input = synthesizePreamble(module) + templateSource + "\n";
        antlr4::ANTLRInputStream inputStream(input);
        CajetaLexer lexer(&inputStream);
        antlr4::CommonTokenStream tokens(&lexer);
        tokens.fill();
        CajetaParser parser(&tokens);
        auto* compUnit = parser.compilationUnit();

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

        // Pre-compute a set of declared type-parameter names — these are the
        // names we'll match formal-parameter identifiers against.
        std::set<string> paramNames;
        for (auto& p : typeParameters) paramNames.insert(p.name);

        // Collect every viable constructor's binding map. A "viable" ctor is
        // one whose arity matches `argTypes`. v1 doesn't do widening / boxing
        // conversions; argument types must hit the same template-parameter
        // slot consistently.
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

        // Single viable binding — assemble args in typeParameter declaration
        // order. Every parameter must have been bound.
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
