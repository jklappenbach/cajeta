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

#include "CajetaClass.h"
#include "QualifiedName.h"
#include "../asn/ClassBodyDeclaration.h"
#include "../compile/CajetaModule.h"
#include "../compile/CajetaLlvmVisitor.h"
#include "../error/Exception.h"
#include "CajetaParser.h"
#include "CajetaLexer.h"

#include "antlr4-runtime/antlr4-runtime.h"

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
        // Non-templates pass through unchanged. Lets callers do
        // `cls->instantiate(args)` without guarding on isTemplate themselves.
        if (!isTemplate()) {
            return static_pointer_cast<CajetaClass>(shared_from_this());
        }

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
            if (param.bounds.empty()) continue;
            auto argClass = dynamic_pointer_cast<CajetaClass>(args[i]);
            for (auto& bound : param.bounds) {
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
        string suffix = buildArgSuffix(args);
        string instCanonical = qName->toCanonical() + suffix;

        auto& structures = module->getStructures();
        auto cached = structures.find(instCanonical);
        if (cached != structures.end()) {
            return cached->second;
        }

        // Re-parse the captured snippet. ANTLR contexts are tied to their
        // parser's lifetime so we don't retain parse trees across compilation
        // phases — we re-parse on demand. The result of this expensive work
        // is cached above; re-parsing only runs once per unique arg list.
        string input = synthesizePreamble(module) + templateSource + "\n";

        antlr4::ANTLRInputStream inputStream(input);
        CajetaLexer lexer(&inputStream);
        antlr4::CommonTokenStream tokens(&lexer);
        tokens.fill();
        CajetaParser parser(&tokens);
        auto* compUnit = parser.compilationUnit();

        // The synthesized input wraps one class declaration. Find it; bail
        // loudly if the snippet structure is somehow wrong (would mean the
        // capture in the visitor produced bad text).
        CajetaParser::ClassDeclarationContext* classDecl = nullptr;
        for (auto* td : compUnit->typeDeclaration()) {
            if (auto* cd = td->classDeclaration()) {
                classDecl = cd;
                break;
            }
        }
        if (!classDecl) {
            throw "template snippet does not contain a classDeclaration";
        }

        // Build the instantiation's qName: same package as the template,
        // simple name carries the arg suffix. Canonical comes out to
        // `pkg.Box<cajeta.int32>` — readable, distinct, and acceptable as
        // an LLVM struct name.
        string instName = qName->getTypeName() + suffix;
        QualifiedNamePtr instQName = QualifiedName::getOrInsert(
            instName, qName->getPackageName());

        // Extends/implements clauses on the template are inherited verbatim
        // for v1. Parameterized supers (`extends Container<T>`) will need
        // their typeArguments substituted and routed through instantiate
        // recursively — that's TPL-5 work. Today, the typeArguments on the
        // super clause are silently dropped (matching the pre-template
        // behavior of QualifiedName::fromContext).
        list<QualifiedNamePtr> instExtended;
        list<QualifiedNamePtr> instImplemented;
        for (auto* tl : classDecl->typeList()) {
            for (auto* tt : tl->typeType()) {
                if (auto* coi = tt->classOrInterfaceType()) {
                    instExtended.push_back(QualifiedName::fromContext(coi));
                }
            }
        }

        auto inst = make_shared<CajetaClass>(module, instQName, instExtended, instImplemented);
        inst->setTypeParameters(typeParameters);   // retained for debugging / introspection
        inst->setTypeArguments(args);

        // Cache BEFORE we walk the body. Lets self-referential templates
        // like `class List<T> { List<T> next; }` resolve their own type
        // during the walk — the second reference finds the partially-built
        // instantiation in the cache instead of recursing forever.
        structures[instCanonical] = inst;

        // Build the substitution map (parameter name → arg) and push it.
        // CajetaType::fromContext consults the top frame, so any `T` in the
        // template body resolves to the concrete arg during this walk.
        map<string, CajetaTypePtr> subst;
        for (size_t i = 0; i < typeParameters.size(); ++i) {
            subst[typeParameters[i].name] = args[i];
        }
        module->pushTypeSubstitution(subst);

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
        CajetaLlvmVisitor visitor(module);
        auto bodyAny = visitor.visitClassBody(classDecl->classBody());
        inst->setClassBody(std::any_cast<ClassBodyDeclarationPtr>(bodyAny));
        inst->generatePrototype();

        CajetaModule::setActiveModule(prevActive);
        stack.clear();
        stack.swap(savedStack);
        module->popTypeSubstitution();

        CajetaModule::getStructureToModule()[instCanonical] = module;
        return inst;
    }

    // --- Diamond inference (TPL-7) ----------------------------------------
    //
    // Walk the template's captured snippet, find constructor declarations,
    // and unify each ctor's parameter types against the supplied argument
    // types. v1 is flat: we look only at the syntactic identifier of each
    // formal parameter's typeType, comparing it directly against type-
    // parameter names. Nested generics in parameter types (`Box<List<T>>(...)`)
    // aren't analyzed.
    //
    // Returns the resolved type arguments in declaration order. Throws on
    // ambiguous overload, conflicting bindings, or any type parameter left
    // unbound.

    static string typeTypeIdentifier(CajetaParser::TypeTypeContext* tt) {
        // For our v1 we only care about the leaf identifier of a non-array,
        // non-primitive typeType. Anything else returns "" so the caller
        // falls back to the "not a type parameter" branch.
        if (!tt || tt->primitiveType()) return "";
        if (!tt->LBRACK().empty()) return "";  // array
        auto* coi = tt->classOrInterfaceType();
        if (!coi) return "";
        const auto& ids = coi->identifier();
        if (ids.size() != 1) return "";        // qualified / nested → not a T
        return ids[0]->getText();
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
        string input = synthesizePreamble(module) + templateSource + "\n";
        antlr4::ANTLRInputStream inputStream(input);
        CajetaLexer lexer(&inputStream);
        antlr4::CommonTokenStream tokens(&lexer);
        tokens.fill();
        CajetaParser parser(&tokens);
        auto* compUnit = parser.compilationUnit();

        CajetaParser::ClassDeclarationContext* classDecl = nullptr;
        for (auto* td : compUnit->typeDeclaration()) {
            if (auto* cd = td->classDeclaration()) {
                classDecl = cd;
                break;
            }
        }
        if (!classDecl) {
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

        for (auto* bdCtx : classDecl->classBody()->classBodyDeclaration()) {
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
                string ident = typeTypeIdentifier(params[i]->typeType());
                if (paramNames.count(ident)) {
                    auto existing = bindings.find(ident);
                    if (existing == bindings.end()) {
                        bindings[ident] = argTypes[i];
                    } else if (existing->second->toCanonical()
                            != argTypes[i]->toCanonical()) {
                        ok = false;   // conflicting bindings for same T
                        break;
                    }
                }
                // Non-type-parameter slots aren't strictly checked in v1.
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
