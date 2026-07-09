//
// Source-synthesis facility (núcleo Layer-1a). See SourceSynthesis.h.
//
#include "cajeta/synth/SourceSynthesis.h"

#include <cctype>

namespace cajeta::synth {

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
