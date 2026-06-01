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
using cajeta::buildtool::loadManifestString;
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
