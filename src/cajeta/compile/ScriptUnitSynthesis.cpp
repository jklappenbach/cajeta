#include "ScriptUnitSynthesis.h"

#include <cctype>
#include <filesystem>

#include "CommonTokenStream.h"

#include "CajetaModule.h"
#include "SessionState.h"
#include "../error/Exception.h"
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

    namespace {

        // A wrapper segment: spliced host text (hostStart > 0) or synthetic
        // glue (hostStart == 0). Text always ends in '\n'.
        struct WrapperSeg {
            std::string text;
            int hostStart = 0;
        };

        int lineCountOf(const std::string& text) {
            int n = 0;
            for (char c : text) {
                if (c == '\n') ++n;
            }
            return n;
        }

    }  // namespace

    std::string synthesizeScriptUnit(antlr4::CommonTokenStream& tokens,
                                     CajetaParser::CompilationUnitContext* ctx,
                                     const std::string& stem,
                                     std::string* outCanonical,
                                     std::vector<std::string>* outBindings,
                                     ScriptLineMap* outLineMap) {
        std::string pkgName = scriptDefaultPackage();
        auto hostLineOf = [](antlr4::ParserRuleContext* c) {
            return (c && c->getStart())
                ? static_cast<int>(c->getStart()->getLine()) : 0;
        };

        std::vector<WrapperSeg> header;
        if (auto* pd = ctx->packageDeclaration()) {
            pkgName = pd->qualifiedName()->getText();
            header.push_back({textOf(tokens, pd) + "\n", hostLineOf(pd)});
        } else {
            header.push_back({"package " + pkgName + ";\n", 0});
        }
        for (auto* imp : ctx->importDeclaration()) {
            header.push_back({textOf(tokens, imp) + "\n", hostLineOf(imp)});
        }

        // One pass over the members, in order: types hoist to top level
        // (relative order preserved), methods become static members, loose
        // statements form the entry body. Hoisting types above the wrapper
        // is safe — top-level siblings have no ordering constraint.
        std::vector<WrapperSeg> hoistedTypes;
        std::vector<WrapperSeg> methods;
        std::vector<WrapperSeg> body;
        CajetaParser::BlockStatementContext* lastStatement = nullptr;
        for (auto* member : ctx->scriptMember()) {
            if (auto* td = member->typeDeclaration()) {
                hoistedTypes.push_back(
                    {textOf(tokens, td) + "\n", hostLineOf(td)});
            } else if (auto* md = member->methodDeclaration()) {
                auto mods = member->modifier();
                std::string line = "    ";
                if (!hasAccessModifier(mods)) line += "public ";
                if (!hasStaticModifier(mods)) line += "static ";
                for (auto* m : mods) {
                    line += m->getText();
                    line += " ";
                }
                // The injected modifiers ride the member's FIRST line, so
                // the whole segment still maps 1:1 from its host start.
                line += textOf(tokens, md);
                line += "\n";
                methods.push_back({std::move(line), hostLineOf(md)});
            } else if (auto* bs = member->blockStatement()) {
                body.push_back(
                    {"        " + textOf(tokens, bs) + "\n", hostLineOf(bs)});
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

        // Assemble in wrapper order, recording where each spliced segment
        // lands (U5 line map — spans stay sorted by construction).
        std::string out;
        int wrapperLine = 1;
        auto append = [&](const WrapperSeg& seg) {
            int lines = lineCountOf(seg.text);
            if (outLineMap && seg.hostStart > 0 && lines > 0) {
                outLineMap->push_back({wrapperLine, seg.hostStart, lines});
            }
            out += seg.text;
            wrapperLine += lines;
        };
        for (auto& seg : header) append(seg);
        for (auto& seg : hoistedTypes) append(seg);
        append({"public final class " + stem + " {\n", 0});
        append({"    public static int32 " + std::string(scriptEntryName())
                    + "() {\n", 0});
        for (auto& seg : body) append(seg);
        if (!endsWithReturn) append({"        return 0;\n", 0});
        append({"    }\n", 0});
        for (auto& seg : methods) append(seg);
        append({"}\n", 0});

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

    void remapScriptException(CajetaModulePtr module, Exception& e) {
        if (!module || !module->isScriptUnit()) return;
        if (e.isScriptRemapped()) return;
        e.markScriptRemapped();
        std::string file = module->scriptDiagFile();
        if (e.hasLocation()) {
            e.setLocation(file, module->mapScriptLine(e.getLine()),
                          e.getColumn());
        } else {
            e.setLocation(file, module->getScriptCurrentHostLine(), 0);
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
