#include "SynthesizedMockClass.h"
#include "Method.h"
#include "../type/FormalParameter.h"
#include "../compile/CajetaLlvmVisitor.h"
#include "../compile/CajetaModule.h"
#include "../asn/ClassBodyDeclaration.h"
#include "../error/Exception.h"
#include "CajetaParser.h"

#include <list>
#include <set>
#include <string>

using namespace std;

namespace cajeta {

    // Canonical names of the scalar primitives. M3 forwards only reference- /
    // void-typed methods; methods touching primitives need boxing/unboxing and
    // are deferred to M4 (left inheriting the real implementation for now).
    static bool isPrimitiveCanonical(const string& c) {
        static const std::set<string> prims = {
            "int8", "int16", "int32", "int64", "int128",
            "uint8", "uint16", "uint32", "uint64", "uint128",
            "float16", "float32", "float64", "float128",
            "boolean", "char"
        };
        return prims.count(c) > 0;
    }

    static string canonicalOf(const CajetaTypePtr& t) {
        if (!t || !t->getQName()) return "";
        return t->getQName()->toCanonical();
    }

    // Build the mock body: a MockEngine field + ctor, then a forwarding override
    // of every overridable reference-/void-signature method on the target. Each
    // override boxes its (reference) args into an Object[], records+answers via
    // engine.handle, and returns the answer INLINE (an owned local would free the
    // stub's value — see docs/mockito-aot.md).
    static string synthesizeMockBody(const CajetaClassPtr& mock,
                                     const CajetaClassPtr& target) {
        string mockName = mock->getQName()->getTypeName();
        string body;
        body += "    public dev.cajeta.unit.MockEngine engine;\n";
        body += "    public " + mockName
              + "() { this.engine = heap dev.cajeta.unit.MockEngine(); }\n";

        for (auto& m : target->getMethodList()) {
            if (!m || m->isConstructor() || m->isStatic()) continue;
            const string& mname = m->getName();
            if (mname.rfind("operator", 0) == 0) continue;   // operators: skip

            CajetaTypePtr rt = m->getReturnType();
            string retCanon = canonicalOf(rt);
            bool isVoid = (!rt || retCanon == "void" || retCanon.empty());
            bool hasPrim = (!isVoid && isPrimitiveCanonical(retCanon));

            string paramDecl;
            string argList;
            for (auto& p : m->getParameterList()) {
                if (!p || p->getName() == "this") continue;
                string ptCanon = canonicalOf(p->getType());
                if (ptCanon.empty() || isPrimitiveCanonical(ptCanon)) {
                    hasPrim = true;
                }
                if (!paramDecl.empty()) paramDecl += ", ";
                paramDecl += ptCanon + " " + p->getName();
                if (!argList.empty()) argList += ", ";
                argList += p->getName();
            }
            if (hasPrim) continue;   // M4

            string retDecl = isVoid ? "void" : retCanon;
            body += "    public " + retDecl + " " + mname + "(" + paramDecl + ") {\n";
            body += "        Object[] a = { " + argList + " };\n";
            if (isVoid) {
                body += "        this.engine.handle(\"" + mname + "\", #a);\n";
            } else {
                body += "        return (" + retCanon
                      + ") this.engine.handle(\"" + mname + "\", #a);\n";
            }
            body += "    }\n";
        }
        return body;
    }

    void fillMockClassBody(const CajetaClassPtr& mock,
                           const CajetaClassPtr& target,
                           const CajetaModulePtr& module) {
        string mockName = mock->getQName()->getTypeName();
        string src = "class " + mockName + " {\n"
                   + synthesizeMockBody(mock, target)
                   + "}\n";

        antlr4::ANTLRInputStream input(src);
        CajetaLexer lexer(&input);
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
                "@GenerateMock: synthesized mock source for '" + mockName
                + "' did not parse as a class declaration",
                "CAJETA_ERROR_MOCK_SYNTH");
        }

        // Walk the synthesized body with the structure stack rooted at the mock,
        // so the field + methods land on it. Mirrors Method::instantiateMethod-
        // TemplateInternal's isolated re-parse.
        auto prevActive = CajetaModule::getActiveModule();
        CajetaModule::setActiveModule(module);
        auto& stack = module->getStructureStack();
        list<CajetaClassPtr> saved;
        saved.swap(stack);
        stack.push_back(mock);

        CajetaLlvmVisitor visitor(module);
        auto bodyAny = visitor.visitClassBody(classDecl->classBody());

        stack.clear();
        stack.swap(saved);
        CajetaModule::setActiveModule(prevActive);

        auto classBody = std::any_cast<ClassBodyDeclarationPtr>(bodyAny);
        mock->setClassBody(classBody);
    }
}
