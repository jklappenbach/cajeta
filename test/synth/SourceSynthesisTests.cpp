//
// Unit 1 of the source-synthesis facility
// (docs/specification/nucleo/source-synthesis-spec.md; plan
// agents/cajeta/nucleo/source-synthesis-plan.md §1). The shared synthesis
// helper's deterministic naming (1.1.4 / 1.2.2): synthesized wrapper/instance
// names must be reproducible (byte-identical across compiles) and collision-
// resistant, replacing the pointer-based `(size_t)this` naming.
//
#include <gtest/gtest.h>
#include <cctype>
#include <string>

#include "cajeta/synth/SourceSynthesis.h"

using cajeta::synth::deriveSynthName;

// 1.1.4 — same inputs synthesize the identical name (reproducible builds).
TEST(SourceSynthesisTests, deriveSynthNameIsDeterministic) {
    auto a = deriveSynthName("MethodTemplateWrapper",
                             "cajeta.codec.json.Json.parse", {"test.Tick"});
    auto b = deriveSynthName("MethodTemplateWrapper",
                             "cajeta.codec.json.Json.parse", {"test.Tick"});
    EXPECT_EQ(a, b);
}

// 1.1.4 — distinct trigger / arg / arity inputs produce distinct names, so two
// specializations never collide in the structure stack / canonical map.
TEST(SourceSynthesisTests, deriveSynthNameVariesByTriggerAndArgs) {
    auto base = deriveSynthName("W", "T.parse", {"Tick"});
    EXPECT_NE(base, deriveSynthName("W", "T.toBytes", {"Tick"}));       // trigger
    EXPECT_NE(base, deriveSynthName("W", "T.parse", {"Quote"}));        // arg type
    EXPECT_NE(base, deriveSynthName("W", "T.parse", {"Tick", "Quote"})); // arity
    EXPECT_NE(base, deriveSynthName("X", "T.parse", {"Tick"}));         // prefix
}

// 1.1.4 — the derived name is always a legal identifier: no '.', '<', '>', '[',
// ']' survive; every character is [A-Za-z0-9_].
TEST(SourceSynthesisTests, deriveSynthNameIsAValidIdentifier) {
    auto n = deriveSynthName("W", "cajeta.codec.json.Json.parse<int8[]>",
                             {"test.Tick"});
    for (char c : n) {
        EXPECT_TRUE(std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            << "non-identifier char in synth name: '" << c << "'";
    }
    EXPECT_EQ(n.find('.'), std::string::npos);
    EXPECT_EQ(n.find('<'), std::string::npos);
    EXPECT_EQ(n.find('['), std::string::npos);
}
