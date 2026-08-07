#include "ScriptUnitSynthesis.h"

#include <cctype>
#include <filesystem>

#include "CommonTokenStream.h"

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

    std::string synthesizeScriptUnit(antlr4::CommonTokenStream& tokens,
                                     CajetaParser::CompilationUnitContext* ctx,
                                     const std::string& stem,
                                     std::string* outCanonical) {
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

}  // namespace cajeta
