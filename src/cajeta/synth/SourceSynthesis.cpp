//
// Source-synthesis facility (núcleo Layer-1a). See SourceSynthesis.h.
//
#include "cajeta/synth/SourceSynthesis.h"
#include "cajeta/synth/SourceSynthesisParse.h"

#include <cctype>

#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/QualifiedName.h"
#include "CajetaLexer.h"
#include "antlr4-runtime/antlr4-runtime.h"

namespace cajeta::synth {

    void injectImportIfUnbound(const CajetaModulePtr& module,
                               const std::string& shortName,
                               const std::string& packageName) {
        if (!module) return;
        auto& imports = module->getImports();
        if (imports.find(shortName) == imports.end()) {
            imports[shortName][packageName] =
                QualifiedName::getOrInsert(shortName, packageName);
        }
    }

    CajetaParser::ClassBodyContext* parseClassBodyFragment(const std::string& src) {
        // Heap-allocated + intentionally leaked: the parse tree outlives this
        // call (later codegen derefs its token pointers). See header.
        auto* input = new antlr4::ANTLRInputStream(src);
        auto* lexer = new CajetaLexer(input);
        auto* tokens = new antlr4::CommonTokenStream(lexer);
        tokens->fill();
        auto* parser = new CajetaParser(tokens);
        return parser->classBody();
    }

    CajetaParser::CompilationUnitContext* parseSynthesizedUnit(const std::string& src) {
        auto* input = new antlr4::ANTLRInputStream(src);
        auto* lexer = new CajetaLexer(input);
        auto* tokens = new antlr4::CommonTokenStream(lexer);
        tokens->fill();
        auto* parser = new CajetaParser(tokens);
        return parser->compilationUnit();
    }

    namespace {
        // Map any non-identifier character to '_' so the derived name is a legal
        // identifier. Deterministic and injective enough for our inputs: the
        // separators between prefix/trigger/args keep distinct component lists
        // from aliasing (arity is encoded by the number of separators).
        std::string sanitize(const std::string& s) {
            std::string out;
            out.reserve(s.size());
            for (char c : s) {
                out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
                    ? c : '_';
            }
            return out;
        }
    }

    std::string deriveSynthName(const std::string& prefix,
                                const std::string& triggerCanonical,
                                const std::vector<std::string>& argCanonicals) {
        std::string name = "__" + sanitize(prefix) + "__" + sanitize(triggerCanonical);
        // Encode arity explicitly so ([]) and ([a,b]) that sanitize to the same
        // concatenation stay distinct, and so a trailing empty arg can't alias.
        name += "__" + std::to_string(argCanonicals.size());
        for (const auto& a : argCanonicals) {
            name += "_" + sanitize(a);
        }
        return name;
    }

}
