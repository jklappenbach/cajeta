#include "ScriptUnitSynthesis.h"

#include <cctype>
#include <filesystem>

#include "CommonTokenStream.h"

#include "CajetaModule.h"
#include "SessionState.h"
#include "../field/HeapField.h"
#include "../method/Method.h"
#include "../type/CajetaType.h"
#include "../type/Scope.h"

namespace cajeta {

    namespace {

        std::string textOf(antlr4::CommonTokenStream& tokens,
                           antlr4::ParserRuleContext* ctx) {
            return tokens.getText(ctx->getSourceInterval());
        }

        // Does this modifier list already carry an access modifier / static?
        bool hasAccessModifier(const std::vector<CajetaParser::ModifierContext*>& mods) {
            for (auto* m : mods) {
                auto t = m->getText();
                if (t == "public" || t == "private" || t == "protected") return true;
            }
            return false;
        }

        bool hasStaticModifier(const std::vector<CajetaParser::ModifierContext*>& mods) {
            for (auto* m : mods) {
                if (m->getText() == "static") return true;
            }
            return false;
        }

    }  // namespace

    bool isScriptUnit(CajetaParser::CompilationUnitContext* ctx) {
        return ctx != nullptr && !ctx->scriptMember().empty();
    }

    std::string scriptClassStem(const std::string& sourcePath) {
        std::string stem = std::filesystem::path(sourcePath).stem().string();
        std::string out;
        out.reserve(stem.size());
        for (char c : stem) {
            out.push_back((std::isalnum(static_cast<unsigned char>(c)) || c == '_')
                              ? c : '_');
        }
        if (out.empty()) out = "script";
        if (std::isdigit(static_cast<unsigned char>(out[0]))) out.insert(out.begin(), '_');
        return out;
    }

    namespace {

        // Collect the declared names of a scriptMember-level local variable
        // declaration — the session bindings (spec §4). Handles both grammar
        // alternatives: `typeType variableDeclarators` and `var ident = expr`.
        void collectBindingNames(CajetaParser::BlockStatementContext* bs,
                                 std::vector<std::string>* out) {
            if (bs == nullptr || out == nullptr) return;
            auto* lvd = bs->localVariableDeclaration();
            if (lvd == nullptr) return;
            if (auto* vds = lvd->variableDeclarators()) {
                for (auto* vd : vds->variableDeclarator()) {
                    if (vd->variableDeclaratorId()
                            && vd->variableDeclaratorId()->identifier()) {
                        out->push_back(
                            vd->variableDeclaratorId()->identifier()->getText());
                    }
                }
            } else if (lvd->identifier()) {
                out->push_back(lvd->identifier()->getText());
            }
        }

    }  // namespace

    std::string synthesizeScriptUnit(antlr4::CommonTokenStream& tokens,
                                     CajetaParser::CompilationUnitContext* ctx,
                                     const std::string& stem,
                                     std::string* outCanonical,
                                     std::vector<std::string>* outBindings) {
        std::string pkgName = scriptDefaultPackage();
        std::string header;
        if (auto* pd = ctx->packageDeclaration()) {
            pkgName = pd->qualifiedName()->getText();
            header += textOf(tokens, pd);
            header += "\n";
        } else {
            header += "package ";
            header += pkgName;
            header += ";\n";
        }
        for (auto* imp : ctx->importDeclaration()) {
            header += textOf(tokens, imp);
            header += "\n";
        }

        // One pass over the members, in order: types hoist to top level
        // (relative order preserved), methods become static members, loose
        // statements form the entry body. Hoisting types above the wrapper
        // is safe — top-level siblings have no ordering constraint.
        std::string hoistedTypes;
        std::string methods;
        std::string body;
        CajetaParser::BlockStatementContext* lastStatement = nullptr;
        for (auto* member : ctx->scriptMember()) {
            if (auto* td = member->typeDeclaration()) {
                hoistedTypes += textOf(tokens, td);
                hoistedTypes += "\n";
            } else if (auto* md = member->methodDeclaration()) {
                auto mods = member->modifier();
                std::string line = "    ";
                if (!hasAccessModifier(mods)) line += "public ";
                if (!hasStaticModifier(mods)) line += "static ";
                for (auto* m : mods) {
                    line += m->getText();
                    line += " ";
                }
                line += textOf(tokens, md);
                methods += line;
                methods += "\n";
            } else if (auto* bs = member->blockStatement()) {
                body += "        ";
                body += textOf(tokens, bs);
                body += "\n";
                lastStatement = bs;
                collectBindingNames(bs, outBindings);
            }
        }

        // Append the default return only when the body's last statement is
        // not already a return (an appended statement after `return` would
        // be unreachable).
        bool endsWithReturn = lastStatement != nullptr
            && lastStatement->getStart() != nullptr
            && lastStatement->getStart()->getType() == CajetaParser::RETURN;

        std::string out;
        out += header;
        out += hoistedTypes;
        out += "public final class ";
        out += stem;
        out += " {\n";
        out += "    public static int32 ";
        out += scriptEntryName();
        out += "() {\n";
        out += body;
        if (!endsWithReturn) out += "        return 0;\n";
        out += "    }\n";
        out += methods;
        out += "}\n";

        if (outCanonical) *outCanonical = pkgName + "." + stem;
        return out;
    }

    // --- U4: the session seam -------------------------------------------

    namespace {

        // Shared gate: the module is a script unit compiling into a session
        // AND the current method is the synthesized entry. Returns the
        // session (or null when the gate fails).
        SessionState* sessionOfEntry(const CajetaModulePtr& module) {
            if (!module || !module->isScriptUnit()) return nullptr;
            SessionState* session = module->getSessionState();
            if (!session) return nullptr;
            auto method = module->getCurrentMethod();
            if (!method || method->getName() != scriptEntryName()) {
                return nullptr;
            }
            return session;
        }

        // Tag a transfer-site note with the unit it happened in, so a later
        // unit's diagnostic can point back across the seam.
        std::string decorateSite(const std::string& note,
                                 const std::string& hostName) {
            if (note.empty() || hostName.empty()) return note;
            return note + " in unit " + hostName;
        }

    }  // namespace

    void seedSessionScope(CajetaModulePtr module) {
        SessionState* session = sessionOfEntry(module);
        if (!session) return;
        ScopePtr scope = module->getScopeStack().peek();
        if (!scope) return;
        for (auto& fact : session->all()) {
            // Resolve the recorded canonical in THIS unit's type world; a
            // miss seeds name-only — the ownership checks don't need the
            // type, and a live read rejects before touching it.
            CajetaTypePtr type = CajetaType::find(fact.typeCanonical);
            auto field =
                std::make_shared<HeapField>(module, fact.name, type);
            field->setSessionSeeded(true);
            scope->putField(field);
            if (fact.moved) scope->demoteToBorrow(fact.name, fact.transferSite);
        }
    }

    void writeBackSessionState(CajetaModulePtr module) {
        SessionState* session = sessionOfEntry(module);
        if (!session) return;
        ScopePtr scope = module->getScopeStack().peek();
        if (!scope) return;
        // Earlier units' bindings: carry this unit's move-state changes.
        // The transfer note is (re)recorded only on a fresh move — a
        // still-moved binding keeps the note of the unit that moved it.
        for (auto& fact : session->all()) {
            bool nowMoved = scope->isBorrow(fact.name);
            if (nowMoved && !fact.moved) {
                fact.transferSite = decorateSite(
                    scope->transferSiteOf(fact.name),
                    module->getScriptHostName());
            } else if (!nowMoved) {
                fact.transferSite.clear();
            }
            fact.moved = nowMoved;
        }
        // This unit's own top-level bindings (a redeclared seeded name lands
        // here too — its Field replaced the seed). put() keeps first-binding
        // order for names that already exist.
        for (auto& name : module->getScriptBindingNames()) {
            FieldPtr field = scope->localBinding(name);
            if (!field || field->isSessionSeeded()) continue;
            SessionBindingFact fact;
            fact.name = name;
            if (field->getType() && field->getType()->getQName()) {
                fact.typeCanonical =
                    field->getType()->getQName()->toCanonical();
            }
            fact.moved = scope->isBorrow(name);
            fact.transferSite = decorateSite(scope->transferSiteOf(name),
                                             module->getScriptHostName());
            session->put(std::move(fact));
        }
    }

    void maybeEmitSessionDisarm(CajetaModulePtr module,
                                const std::string& name) {
        if (!module || !module->isScriptUnit()) return;
        if (!module->isScriptBindingName(name)) return;
        auto* builder = module->getBuilder();
        if (!builder || !builder->GetInsertBlock()) return;
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        if (!parentFn
            || parentFn->getName().find(scriptEntryName())
                   == llvm::StringRef::npos) {
            return;
        }
        llvm::Function* disarmFn =
            module->getRuntimeFunction("__cajeta_session_disarm");
        if (!disarmFn) return;
        builder->CreateCall(disarmFn, {builder->CreateGlobalString(name)});
    }

}  // namespace cajeta
