//
// Method::instantiateMethodTemplate(args) — per-call monomorphization of a
// method-templated declaration into a concrete Method (cajeta-docs/stdlib/
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
#include "../compile/CajetaModule.h"
#include "../compile/CajetaLlvmVisitor.h"
#include "../error/Exception.h"
#include "CajetaParser.h"
#include "CajetaLexer.h"

#include "antlr4-runtime/antlr4-runtime.h"

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
        for (size_t i = 0; i < methodTypeParameters.size(); ++i) {
            const auto& param = methodTypeParameters[i];
            if (param.bounds.empty()) continue;
            auto argClass = std::dynamic_pointer_cast<CajetaClass>(args[i]);
            for (auto& bound : param.bounds) {
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

        // Cache hit?
        std::string suffix = buildMethodArgSuffix(args);
        auto cached = methodInstantiationCache.find(suffix);
        if (cached != methodInstantiationCache.end()) {
            return cached->second;
        }

        if (methodSource.empty()) {
            throw Exception(
                "method template '" + name + "' has no captured source; "
                "method-level templates must be parsed via the standard "
                "visitor path before instantiation",
                "CAJETA_ERROR_METHOD_TEMPLATE_INVALID");
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
            + methodSource + "\n"
            + "}\n";

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
        inst->setMethodTypeParameters(methodTypeParameters);
        inst->setMethodTypeArguments(args);
        // Reparent to the original template's parent so downstream
        // canonical-name building uses the right class name. The visitor
        // set it to the wrapper class.
        inst->setParentForInstantiation(parent);
        // Keep the bare method name. The concrete value-param types
        // usually make the instantiation's toCanonical/toGeneric
        // distinct from the template's, so map keys + LLVM symbols stay
        // unique. The exception is a templated method whose T-vars
        // appear ONLY outside the value-param signature (e.g.
        // `static <T> int32 sizeOf()`) — there, multiple instantiations
        // would all share `Util::sizeOf()` and collide in addMethod's
        // duplicate-static check. That case is documented as a known
        // limitation; mangling the name to disambiguate breaks lambda-
        // expectedType propagation (the propagator looks up by bare
        // name and would only find the template's placeholder formals).

        methodInstantiationCache[suffix] = inst;
        return inst;
    }

}  // namespace cajeta
