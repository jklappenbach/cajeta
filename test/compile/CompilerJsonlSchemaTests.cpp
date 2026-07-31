// compiler-jsonl Unit 6: the committed schema cannot silently rot.
//
// A schema doc that is merely written is a schema doc that drifts the first
// time someone adds an emitter. So the ground truth here is the SOURCE: every
// record kind the compiler can open must be documented, and every documented
// kind must still exist. Adding `openRecord("newthing")` fails this test until
// the schema learns about it — which is the whole point.
//
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#include <llvm/Support/JSON.h>

#include "cajeta/error/Diagnostics.h"

namespace fs = std::filesystem;

namespace {

fs::path sourceRoot() {
    const char* env = std::getenv("CAJETA_SOURCE_ROOT");
    if (env && *env) return fs::path(env);
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
    return fs::path(CAJETA_SOURCE_ROOT_DEFAULT);
#else
    return fs::path(".");
#endif
}

fs::path schemaPath() {
    return sourceRoot() / "specs" / "schemas" / "compiler-jsonl.schema.json";
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}

// Every literal passed to openRecord(...) anywhere under src/. That call is the
// single place a record is opened (Diagnostics.cpp), so this is the complete
// set of kinds the compiler's own emitters can produce.
std::set<std::string> kindsEmittedInSource() {
    std::set<std::string> kinds;
    const std::regex re(R"(openRecord\(\"([a-zA-Z]+)\"\))");
    for (fs::recursive_directory_iterator it(sourceRoot() / "src"), end;
             it != end; ++it) {
        if (!it->is_regular_file()) continue;
        const auto ext = it->path().extension();
        if (ext != ".cpp" && ext != ".h") continue;
        const std::string text = readAll(it->path());
        for (std::sregex_iterator m(text.begin(), text.end(), re), mend;
                 m != mend; ++m) {
            kinds.insert((*m)[1].str());
        }
    }
    // The xref sub-stream predates the shared emitter and writes its records by
    // hand in XrefIndex.cpp (spec 3.1.4 folds it in at the next major). It is
    // part of the format regardless, so the schema must document it.
    kinds.insert("xref");
    return kinds;
}

std::set<std::string> kindsDocumentedInSchema(const std::string& text,
                                              std::string* parseError) {
    std::set<std::string> kinds;
    auto parsed = llvm::json::parse(text);
    if (!parsed) {
        *parseError = llvm::toString(parsed.takeError());
        return kinds;
    }
    auto* obj = parsed->getAsObject();
    if (!obj) { *parseError = "schema root is not an object"; return kinds; }
    auto* defs = obj->getObject("$defs");
    if (!defs) { *parseError = "schema has no $defs"; return kinds; }
    for (const auto& kv : *defs) kinds.insert(kv.first.str());
    return kinds;
}

} // namespace

TEST(CompilerJsonlSchema, SchemaFileExistsAndParses) {
    ASSERT_TRUE(fs::exists(schemaPath()))
        << "no schema at " << schemaPath();
    std::string err;
    auto kinds = kindsDocumentedInSchema(readAll(schemaPath()), &err);
    EXPECT_TRUE(err.empty()) << "schema does not parse: " << err;
    EXPECT_FALSE(kinds.empty()) << "schema documents no record kinds";
}

// 6.1.1 — the doc cannot fall behind the code.
TEST(CompilerJsonlSchema, EveryKindTheCompilerEmitsIsDocumented) {
    ASSERT_TRUE(fs::exists(schemaPath()));
    std::string err;
    const auto documented = kindsDocumentedInSchema(readAll(schemaPath()), &err);
    ASSERT_TRUE(err.empty()) << err;
    const auto emitted = kindsEmittedInSource();
    ASSERT_FALSE(emitted.empty()) << "found no openRecord() sites — the scan "
                                     "is broken, not the schema";
    for (const auto& k : emitted) {
        EXPECT_TRUE(documented.count(k) > 0)
            << "the compiler can emit `" << k << "` but the schema does not "
               "document it (" << schemaPath() << ")";
    }
}

// ...nor ahead of it: a kind that no longer exists is a lie in the other
// direction, and the reader that trusts it will wait forever for a record.
TEST(CompilerJsonlSchema, EveryDocumentedKindStillExists) {
    ASSERT_TRUE(fs::exists(schemaPath()));
    std::string err;
    const auto documented = kindsDocumentedInSchema(readAll(schemaPath()), &err);
    ASSERT_TRUE(err.empty()) << err;
    const auto emitted = kindsEmittedInSource();
    for (const auto& k : documented) {
        EXPECT_TRUE(emitted.count(k) > 0)
            << "the schema documents `" << k << "` but nothing emits it";
    }
}

// The schema states the version it describes, and it must be the version the
// compiler actually stamps into every stream record.
TEST(CompilerJsonlSchema, SchemaVersionMatchesTheEmittedVersion) {
    ASSERT_TRUE(fs::exists(schemaPath()));
    auto parsed = llvm::json::parse(readAll(schemaPath()));
    ASSERT_TRUE(!!parsed) << "schema does not parse";
    auto* obj = parsed->getAsObject();
    ASSERT_NE(obj, nullptr);
    auto* version = obj->getObject("version");
    ASSERT_NE(version, nullptr) << "schema declares no version";
    auto major = version->getInteger("major");
    auto minor = version->getInteger("minor");
    ASSERT_TRUE(major.has_value() && minor.has_value());
    EXPECT_EQ(*major, cajeta::kJsonlSchemaMajor)
        << "schema major disagrees with what the compiler emits";
    EXPECT_EQ(*minor, cajeta::kJsonlSchemaMinor)
        << "schema minor disagrees with what the compiler emits";
}
