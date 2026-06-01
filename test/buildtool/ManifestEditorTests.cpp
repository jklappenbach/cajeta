// Regression tests for ManifestEditor — the helpers that
// add/remove `settings.dependencies` entries in a manifest's
// JSONC source while preserving the rest of the file verbatim.
//
// The contract these tests pin:
//   - Existing entries get their constraint updated in-place.
//   - New entries land inside the existing dependencies block.
//   - A missing dependencies subobject is created under settings.
//   - A missing settings block is created at the root.
//   - Remove errors with a clear message when the dep isn't present.
//   - Every edit produces a manifest that re-parses cleanly via
//     loadManifestString — the helpers re-validate before returning.

#include "cajeta/buildtool/ManifestEditor.h"
#include "cajeta/buildtool/Manifest.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <string>

using cajeta::buildtool::addDependencyToManifest;
using cajeta::buildtool::appendCoverageExclude;
using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::removeCoverageExclude;
using cajeta::buildtool::removeDependencyFromManifest;

namespace {

    std::string errorText(llvm::Error&& err) {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << err;
        consumeError(std::move(err));
        return out;
    }

    // Helper: parse the result and pull out the constraint for a
    // declared dep (empty when not present).
    std::string declaredConstraint(const std::string& src,
                                   const std::string& depName) {
        auto m = loadManifestString(src);
        if (!m) {
            consumeError(m.takeError());
            return "<load-failed>";
        }
        const auto* deps = m->settingsRaw.getObject("dependencies");
        if (!deps) return "";
        const auto* v = deps->get(depName);
        if (!v) return "";
        if (auto s = v->getAsString()) return s->str();
        return "";
    }

} // namespace

// ─── add ──────────────────────────────────────────────────────────────

TEST(ManifestEditorTests, addUpdatesExistingDependencyInPlace) {
    std::string src = R"({
    "details": { "name": "a.b", "version": "0.1" },
    "settings": {
        "dependencies": {
            "acme.lib": "1.0.0"
        }
    }
})";
    auto out = addDependencyToManifest(src, "acme.lib", "1.5.0");
    ASSERT_TRUE((bool)out) << errorText(out.takeError());
    EXPECT_EQ(declaredConstraint(*out, "acme.lib"), "1.5.0");
}

TEST(ManifestEditorTests, addAppendsToExistingDependenciesBlock) {
    std::string src = R"({
    "details": { "name": "a.b", "version": "0.1" },
    "settings": {
        "dependencies": {
            "acme.lib": "1.0.0"
        }
    }
})";
    auto out = addDependencyToManifest(src, "new.pkg", "2.0.0");
    ASSERT_TRUE((bool)out) << errorText(out.takeError());
    EXPECT_EQ(declaredConstraint(*out, "acme.lib"), "1.0.0");
    EXPECT_EQ(declaredConstraint(*out, "new.pkg"), "2.0.0");
}

TEST(ManifestEditorTests, addCreatesDependenciesUnderExistingSettings) {
    std::string src = R"({
    "details": { "name": "a.b", "version": "0.1" },
    "settings": {
        "build": { "target": "host" }
    }
})";
    auto out = addDependencyToManifest(src, "fresh.pkg", "1.0.0");
    ASSERT_TRUE((bool)out) << errorText(out.takeError());
    EXPECT_EQ(declaredConstraint(*out, "fresh.pkg"), "1.0.0");

    // The pre-existing build block survives the edit.
    auto m = loadManifestString(*out);
    ASSERT_TRUE((bool)m) << errorText(m.takeError());
    const auto* build = m->settingsRaw.getObject("build");
    ASSERT_NE(build, nullptr);
    EXPECT_EQ(build->getString("target").value_or("").str(), "host");
}

TEST(ManifestEditorTests, addCreatesSettingsAtRoot) {
    std::string src = R"({
    "details": { "name": "a.b", "version": "0.1" }
})";
    auto out = addDependencyToManifest(src, "fresh.pkg", "1.0.0");
    ASSERT_TRUE((bool)out) << errorText(out.takeError());
    EXPECT_EQ(declaredConstraint(*out, "fresh.pkg"), "1.0.0");
}

TEST(ManifestEditorTests, addInjectsIntoEmptyDependenciesBlock) {
    std::string src = R"({
    "details": { "name": "a.b", "version": "0.1" },
    "settings": {
        "dependencies": {}
    }
})";
    auto out = addDependencyToManifest(src, "pkg", "1.0.0");
    ASSERT_TRUE((bool)out) << errorText(out.takeError());
    EXPECT_EQ(declaredConstraint(*out, "pkg"), "1.0.0");
}

TEST(ManifestEditorTests, addPreservesLineCommentsOutsideEditArea) {
    std::string src = R"({
    // top-of-file comment
    "details": { "name": "a.b", "version": "0.1" },
    "settings": {
        // this comment must survive the edit
        "dependencies": {
            "acme.lib": "1.0.0"
        }
    }
})";
    auto out = addDependencyToManifest(src, "new.pkg", "1.5.0");
    ASSERT_TRUE((bool)out) << errorText(out.takeError());
    EXPECT_NE(out->find("top-of-file comment"), std::string::npos);
    EXPECT_NE(out->find("this comment must survive"), std::string::npos);
    EXPECT_EQ(declaredConstraint(*out, "new.pkg"), "1.5.0");
}

// ─── remove ───────────────────────────────────────────────────────────

TEST(ManifestEditorTests, removeDeletesDeclaredDependency) {
    std::string src = R"({
    "details": { "name": "a.b", "version": "0.1" },
    "settings": {
        "dependencies": {
            "keep.this": "1.0.0",
            "drop.this": "2.0.0"
        }
    }
})";
    auto out = removeDependencyFromManifest(src, "drop.this");
    ASSERT_TRUE((bool)out) << errorText(out.takeError());
    EXPECT_EQ(declaredConstraint(*out, "keep.this"), "1.0.0");
    EXPECT_EQ(declaredConstraint(*out, "drop.this"), "");
}

TEST(ManifestEditorTests, removeWorksOnLastEntry) {
    std::string src = R"({
    "details": { "name": "a.b", "version": "0.1" },
    "settings": {
        "dependencies": {
            "only.entry": "1.0.0"
        }
    }
})";
    auto out = removeDependencyFromManifest(src, "only.entry");
    ASSERT_TRUE((bool)out) << errorText(out.takeError());
    EXPECT_EQ(declaredConstraint(*out, "only.entry"), "");
    // Result still parses as a valid manifest with an empty deps block.
    auto m = loadManifestString(*out);
    ASSERT_TRUE((bool)m) << errorText(m.takeError());
}

TEST(ManifestEditorTests, removeErrorsWhenDepNotDeclared) {
    std::string src = R"({
    "details": { "name": "a.b", "version": "0.1" },
    "settings": {
        "dependencies": {
            "exists": "1.0.0"
        }
    }
})";
    auto out = removeDependencyFromManifest(src, "missing");
    ASSERT_FALSE((bool)out);
    auto msg = errorText(out.takeError());
    EXPECT_NE(msg.find("missing"), std::string::npos);
    EXPECT_NE(msg.find("not declared"), std::string::npos);
}

TEST(ManifestEditorTests, removeErrorsWhenNoDependenciesBlock) {
    std::string src = R"({
    "details": { "name": "a.b", "version": "0.1" }
})";
    auto out = removeDependencyFromManifest(src, "anything");
    ASSERT_FALSE((bool)out);
    auto msg = errorText(out.takeError());
    EXPECT_NE(msg.find("not declared"), std::string::npos);
}

TEST(ManifestEditorTests, roundTripAddThenRemoveReturnsSameDecls) {
    std::string src = R"({
    "details": { "name": "a.b", "version": "0.1" },
    "settings": {
        "dependencies": {
            "keep": "1.0.0"
        }
    }
})";
    auto added = addDependencyToManifest(src, "transient", "2.0.0");
    ASSERT_TRUE((bool)added) << errorText(added.takeError());
    EXPECT_EQ(declaredConstraint(*added, "transient"), "2.0.0");

    auto removed = removeDependencyFromManifest(*added, "transient");
    ASSERT_TRUE((bool)removed) << errorText(removed.takeError());
    EXPECT_EQ(declaredConstraint(*removed, "keep"), "1.0.0");
    EXPECT_EQ(declaredConstraint(*removed, "transient"), "");
}

// ─── coverage exclude mutators ──────────────────────────────────────

namespace {

    // Returns the typed exclude entries from the resulting manifest
    // as kind+pattern+reason triples. Empty when none declared.
    struct ExcludeRow { std::string kind, pattern, reason; };
    std::vector<ExcludeRow> readExcludes(const std::string& src) {
        std::vector<ExcludeRow> out;
        auto m = loadManifestString(src);
        if (!m) { consumeError(m.takeError()); return out; }
        const auto* cov = m->pluginsRaw.getObject("cajeta.coverage");
        if (!cov) return out;
        const auto* config = cov->getObject("config");
        if (!config) return out;
        const auto* arr = config->getArray("exclude");
        if (!arr) return out;
        for (const auto& v : *arr) {
            ExcludeRow r;
            if (const auto* o = v.getAsObject()) {
                if (auto s = o->getString("kind"))    r.kind = s->str();
                if (auto s = o->getString("pattern")) r.pattern = s->str();
                if (auto s = o->getString("reason"))  r.reason = s->str();
            } else if (auto s = v.getAsString()) {
                r.kind = "file"; r.pattern = s->str();
            }
            out.push_back(std::move(r));
        }
        return out;
    }

    constexpr const char* kCoverageManifestNoExclude = R"({
    "details": { "name": "p", "version": "0.1.0" },
    "plugins": {
        "cajeta.coverage": {
            "version": "1.0.*",
            "config": {
                "grain": "line",
                "min": 80
            }
        }
    }
})";

    constexpr const char* kCoverageManifestNoConfig = R"({
    "details": { "name": "p", "version": "0.1.0" },
    "plugins": {
        "cajeta.coverage": {
            "version": "1.0.*"
        }
    }
})";

    constexpr const char* kCoverageManifestNoPlugin = R"({
    "details": { "name": "p", "version": "0.1.0" }
})";

} // namespace

TEST(ManifestEditorTests, coverageAppendCreatesExcludeArrayInline) {
    auto out = appendCoverageExclude(
        kCoverageManifestNoExclude, "file",
        "**/*_generated.cajeta", "machine-generated");
    ASSERT_TRUE((bool)out) << errorText(out.takeError());
    auto rows = readExcludes(*out);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].kind,    "file");
    EXPECT_EQ(rows[0].pattern, "**/*_generated.cajeta");
    EXPECT_EQ(rows[0].reason,  "machine-generated");
}

TEST(ManifestEditorTests, coverageAppendCreatesConfigBlock) {
    auto out = appendCoverageExclude(
        kCoverageManifestNoConfig, "symbol",
        "com.foo.Bar.baz", "trivial accessor");
    ASSERT_TRUE((bool)out) << errorText(out.takeError());
    auto rows = readExcludes(*out);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].kind, "symbol");
}

TEST(ManifestEditorTests, coverageAppendAppendsToExistingArray) {
    auto first = appendCoverageExclude(
        kCoverageManifestNoExclude, "file",
        "**/*_generated.cajeta", "machine-generated");
    ASSERT_TRUE((bool)first);
    auto second = appendCoverageExclude(
        *first, "symbol",
        "com.foo.Bar.baz", "trivial accessor; tested everywhere");
    ASSERT_TRUE((bool)second) << errorText(second.takeError());
    auto rows = readExcludes(*second);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].pattern, "**/*_generated.cajeta");
    EXPECT_EQ(rows[1].pattern, "com.foo.Bar.baz");
}

TEST(ManifestEditorTests, coverageAppendRefusesDuplicateKindAndPattern) {
    auto first = appendCoverageExclude(
        kCoverageManifestNoExclude, "file",
        "**/*_generated.cajeta", "machine-generated");
    ASSERT_TRUE((bool)first);
    auto dup = appendCoverageExclude(
        *first, "file",
        "**/*_generated.cajeta", "different reason");
    ASSERT_FALSE((bool)dup);
    auto msg = errorText(dup.takeError());
    EXPECT_NE(msg.find("already present"), std::string::npos);
}

TEST(ManifestEditorTests, coverageAppendAllowsDifferentKindSamePattern) {
    auto first = appendCoverageExclude(
        kCoverageManifestNoExclude, "file",
        "com.foo.*", "files in foo");
    ASSERT_TRUE((bool)first);
    auto second = appendCoverageExclude(
        *first, "package",
        "com.foo.*", "package foo");
    ASSERT_TRUE((bool)second) << errorText(second.takeError());
    auto rows = readExcludes(*second);
    EXPECT_EQ(rows.size(), 2u);
}

TEST(ManifestEditorTests, coverageAppendRefusesWhenPluginNotDeclared) {
    auto out = appendCoverageExclude(
        kCoverageManifestNoPlugin, "file", "x", "y");
    ASSERT_FALSE((bool)out);
    auto msg = errorText(out.takeError());
    EXPECT_NE(msg.find("no cajeta.coverage plugin"), std::string::npos);
}

TEST(ManifestEditorTests, coverageAppendRefusesUnknownKind) {
    auto out = appendCoverageExclude(
        kCoverageManifestNoExclude, "function", "x", "y");
    ASSERT_FALSE((bool)out);
}

TEST(ManifestEditorTests, coverageRemoveDeletesMatchingPattern) {
    auto added = appendCoverageExclude(
        kCoverageManifestNoExclude, "file",
        "**/*_generated.cajeta", "machine-generated");
    ASSERT_TRUE((bool)added);
    auto removed = removeCoverageExclude(
        *added, "**/*_generated.cajeta");
    ASSERT_TRUE((bool)removed) << errorText(removed.takeError());
    EXPECT_EQ(removed->count, 1);
    EXPECT_TRUE(readExcludes(removed->newSource).empty());
}

TEST(ManifestEditorTests, coverageRemoveErrorsWhenPatternNotFound) {
    auto out = removeCoverageExclude(
        kCoverageManifestNoExclude, "missing");
    ASSERT_FALSE((bool)out);
}

TEST(ManifestEditorTests, coverageRemoveAcrossKindsByPattern) {
    auto first = appendCoverageExclude(
        kCoverageManifestNoExclude, "file",
        "com.foo.*", "f");
    ASSERT_TRUE((bool)first);
    auto second = appendCoverageExclude(
        *first, "package",
        "com.foo.*", "p");
    ASSERT_TRUE((bool)second);
    auto removed = removeCoverageExclude(*second, "com.foo.*");
    ASSERT_TRUE((bool)removed) << errorText(removed.takeError());
    EXPECT_EQ(removed->count, 2);
    EXPECT_TRUE(readExcludes(removed->newSource).empty());
}

TEST(ManifestEditorTests, coverageRoundTripAppendThenRemove) {
    auto added = appendCoverageExclude(
        kCoverageManifestNoExclude, "symbol",
        "com.foo.Bar.baz", "trivial accessor");
    ASSERT_TRUE((bool)added);
    auto removed = removeCoverageExclude(
        *added, "com.foo.Bar.baz");
    ASSERT_TRUE((bool)removed) << errorText(removed.takeError());
    // After remove, re-add should succeed (no dangling state).
    auto reAdded = appendCoverageExclude(
        removed->newSource, "symbol",
        "com.foo.Bar.baz", "trivial accessor");
    ASSERT_TRUE((bool)reAdded) << errorText(reAdded.takeError());
}

TEST(ManifestEditorTests, coverageAppendPreservesComments) {
    std::string src = R"({
    "details": { "name": "p", "version": "0.1.0" },
    "plugins": {
        // The coverage plugin gates CI on min %.
        "cajeta.coverage": {
            "version": "1.0.*",
            "config": {
                "grain": "line",  // line-grain probes
                "min": 80
            }
        }
    }
})";
    auto out = appendCoverageExclude(
        src, "file", "**/*.gen", "generated");
    ASSERT_TRUE((bool)out) << errorText(out.takeError());
    EXPECT_NE(out->find("// The coverage plugin gates CI on min %."),
              std::string::npos);
    EXPECT_NE(out->find("// line-grain probes"), std::string::npos);
}
