//
// Unit 2 of the source-synthesis facility
// (docs/specification/nucleo/source-synthesis-spec.md §2; plan
// agents/cajeta/nucleo/source-synthesis-plan.md §2). The trigger registry's
// body-dispatch mechanics: a body trigger resolves to exactly one synthesizer
// (2.1.1), no match falls through to the failsafe (2.1.2), and two matches are
// a loud error with no silent precedence (2.1.3). Tested with fake
// synthesizers on a local registry so the compiler-wide instance() (which
// carries the real codecs) is untouched.
//
#include <gtest/gtest.h>
#include <optional>
#include <string>

#include "cajeta/synth/SynthesizerRegistry.h"
#include "cajeta/error/Exception.h"

using cajeta::synth::SynthesizerRegistry;
using cajeta::synth::SynthesisContext;

namespace {
    // A fake body synthesizer that claims the declaration iff methodName == want.
    cajeta::synth::BodySynthesizer claiming(std::string want, std::string body) {
        return [want, body](const SynthesisContext& c) -> std::optional<std::string> {
            if (c.methodName == want) return body;
            return std::nullopt;
        };
    }
    SynthesisContext ctxFor(const std::string& method) {
        SynthesisContext c;
        c.methodName = method;
        return c;
    }
}

// 2.1.1 — a registered body synthesizer dispatches for its trigger.
TEST(SynthesizerRegistryTests, dispatchBodyReturnsTheSingleMatch) {
    SynthesizerRegistry reg;
    reg.registerBody("json", claiming("parse", "JSON_BODY"));
    reg.registerBody("csv",  claiming("read",  "CSV_BODY"));
    auto r = reg.dispatchBody(ctxFor("parse"));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "JSON_BODY");
}

// 2.1.2 — no registered synthesizer matches → nullopt (the failsafe; the caller
// keeps the captured/declared source).
TEST(SynthesizerRegistryTests, dispatchBodyFallsThroughWhenNoMatch) {
    SynthesizerRegistry reg;
    reg.registerBody("json", claiming("parse", "JSON_BODY"));
    EXPECT_FALSE(reg.dispatchBody(ctxFor("unrelated")).has_value());
}

// 2.1.3 — two synthesizers both claiming the same declaration is a loud error,
// naming both; no silent first-match precedence.
TEST(SynthesizerRegistryTests, dispatchBodyTwoMatchesIsALoudError) {
    SynthesizerRegistry reg;
    reg.registerBody("alpha", claiming("x", "A"));
    reg.registerBody("beta",  claiming("x", "B"));
    try {
        reg.dispatchBody(ctxFor("x"));
        FAIL() << "expected an ambiguity error";
    } catch (cajeta::Exception& e) {
        std::string msg = e.getMessage();
        EXPECT_NE(msg.find("alpha"), std::string::npos);
        EXPECT_NE(msg.find("beta"), std::string::npos);
    }
}

// An empty registry always falls through (degenerate failsafe).
TEST(SynthesizerRegistryTests, emptyRegistryFallsThrough) {
    SynthesizerRegistry reg;
    EXPECT_EQ(reg.bodyCount(), 0u);
    EXPECT_FALSE(reg.dispatchBody(ctxFor("anything")).has_value());
}

namespace {
    cajeta::synth::MemberSynthesizer memberClaiming(std::string want, std::string frag) {
        return [want, frag](const SynthesisContext& c)
                -> std::optional<cajeta::synth::MemberSynthesisResult> {
            if (c.methodName != want) return std::nullopt;
            cajeta::synth::MemberSynthesisResult r;
            r.classBodyFragment = frag;
            return r;
        };
    }
}

// 3.1.3 (mechanic) — member synthesizers COMPOSE: several may inject into one
// declaration, and collectMembers returns every one that claims the target.
TEST(SynthesizerRegistryTests, collectMembersComposesAllClaimers) {
    SynthesizerRegistry reg;
    reg.registerMember("a", memberClaiming("T", "{ int a; }"));
    reg.registerMember("b", memberClaiming("T", "{ int b; }"));
    reg.registerMember("c", memberClaiming("OTHER", "{ int c; }"));  // declines
    auto claimed = reg.collectMembers(ctxFor("T"));
    ASSERT_EQ(claimed.size(), 2u);
    EXPECT_EQ(claimed[0].first, "a");
    EXPECT_EQ(claimed[1].first, "b");
}

// A member synthesizer that declines contributes nothing (failsafe).
TEST(SynthesizerRegistryTests, collectMembersEmptyWhenNoneClaim) {
    SynthesizerRegistry reg;
    reg.registerMember("a", memberClaiming("T", "{ int a; }"));
    EXPECT_TRUE(reg.collectMembers(ctxFor("nope")).empty());
}

// 3.1.4 (mechanic) — validate-first: a member synthesizer that rejects its
// trigger throws, and collectMembers propagates it (the caller surfaces a
// user-attributed compile error and injects nothing).
TEST(SynthesizerRegistryTests, collectMembersPropagatesValidateFirstThrow) {
    SynthesizerRegistry reg;
    reg.registerMember("strict", [](const SynthesisContext&)
            -> std::optional<cajeta::synth::MemberSynthesisResult> {
        throw cajeta::Exception("bad trigger args", "CAJETA_ERROR_SYNTH_VALIDATE");
    });
    EXPECT_THROW(reg.collectMembers(ctxFor("T")), cajeta::Exception);
}
