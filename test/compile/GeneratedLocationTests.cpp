// diagnostic-location Unit 2 (spec 3, 6.2, 6.3): a diagnostic inside generated code points at the
// declaration that asked for it, names the generator in `via`, and keeps the generator's position in `from`.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <string>

#include <llvm/Support/JSON.h>

#include "cajeta/error/Diagnostics.h"
#include "cajeta/error/GeneratedCode.h"

namespace fs = std::filesystem;
using namespace cajeta;

namespace {

fs::path sourceRoot() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    if (envRoot && *envRoot) return envRoot;
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
    return CAJETA_SOURCE_ROOT_DEFAULT;
#else
    return ".";
#endif
}

bool has(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

std::string emitted(const Exception& e) {
    testing::internal::CaptureStderr();
    emitJsonDiagnostic("error", e.getErrorId(), e.getMessage(), e.getFile(), e.getLine(),
                       e.getColumn(), "project", e.getGenerated());
    return testing::internal::GetCapturedStderr();
}

// Compiles `source` as src/test/Use.cajeta and returns the compiler's combined output.
struct Fixture {
    fs::path root;
    std::string file;
    std::string out;
};

Fixture run(const std::string& source, const std::string& mode) {
    Fixture f;
    std::string bin = (sourceRoot() / "build" / "src" / "cajeta").string();
    if (!fs::exists(bin)) return f;
    static std::mt19937_64 rng(std::random_device{}());
    f.root = fs::temp_directory_path() / ("cajeta_gen_" + std::to_string(rng()));
    fs::create_directories(f.root / "src" / "test");
    fs::create_directories(f.root / "ar");
    std::ofstream(f.root / "src" / "test" / "Use.cajeta") << source;
    auto out = f.root / "out.txt";
    std::string src = (f.root / "src").string();
    std::string cmd;
    if (mode == "lint") {
        f.file = "test/Use.cajeta";
        cmd = bin + " --lint " + src + " --emit-xref=" + (f.root / "x.json").string()
            + " --diag-format=json";
    } else {
        f.file = src + "/test/Use.cajeta";
        cmd = bin + (mode == "json" ? " --diag-format=json" : "") + " test.Use.f " + src + " "
            + (f.root / "ar").string();
    }
    (void) std::system((cmd + " > " + out.string() + " 2>&1").c_str());
    std::ifstream in(out);
    f.out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return f;
}

std::string at(const Fixture& f, int line) {
    return "\"file\":\"" + f.file + "\",\"line\":" + std::to_string(line) + ",";
}

std::string from(const std::string& file, int line) {
    return "\"from\":{\"file\":\"" + file + "\",\"line\":" + std::to_string(line) + ",";
}

const char* kBox =
    "package test;\n"
    "\n"
    "public class Cell {\n"
    "    public int64 n;\n"
    "    public Cell(int64 v) { this.n = v; }\n"
    "}\n"
    "\n"
    "public class Box<T> {\n"
    "    public T v;\n"
    "    public Box(T x) { this.v #= x; }\n"
    "    public int64 weigh() { return v.weight(); }\n"
    "}\n"
    "\n"
    "public final class Use {\n"
    "    public static int64 f() {\n"
    "        Box<Cell> b = heap Box<Cell>(heap Cell(3));\n"
    "        return b.weigh();\n"
    "    }\n"
    "}\n";

}  // namespace

// 6.2: `via` is free text in the schema, so a new generator needs no schema change.
TEST(GeneratedLocation, viaIsFreeTextInTheSchema) {
    std::ifstream in(sourceRoot() / "specs" / "schemas" / "compiler-jsonl.schema.json");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto schema = llvm::json::parse(text);
    ASSERT_TRUE((bool) schema) << llvm::toString(schema.takeError());
    const auto* via = schema->getAsObject()->getObject("$defs")->getObject("diagnostic")
                          ->getObject("properties")->getObject("via");
    ASSERT_NE(via, nullptr);
    EXPECT_EQ(via->getString("type"), llvm::StringRef("string"));
    EXPECT_EQ(via->get("enum"), nullptr);
}

// 6.2: a body synthesizer is named by its registry label, in kebab case.
TEST(GeneratedLocation, aSynthesizerIsNamedByItsLabel) {
    EXPECT_EQ(synthesizerVia("json"), "json-synthesizer");
    EXPECT_EQ(synthesizerVia("csv"), "csv-synthesizer");
    EXPECT_EQ(synthesizerVia("protobuf"), "protobuf-synthesizer");
    EXPECT_EQ(synthesizerVia("ion"), "ion-synthesizer");
    EXPECT_EQ(synthesizerVia("avro"), "avro-synthesizer");
    EXPECT_EQ(synthesizerVia("frameCsv"), "frame-csv-synthesizer");
}

TEST(GeneratedLocation, aNewGeneratorNameIsWrittenVerbatim) {
    GenerationSite site;
    site.via = synthesizerVia("protobuf");
    site.request = {"test/Msg.cajeta", 4, 14};
    GenerationScope scope(site);
    std::string out = emitted(Exception("m", "CAJETA_ERROR_X"));
    EXPECT_TRUE(has(out, "\"at\":\"generated\",\"via\":\"protobuf-synthesizer\"")) << out;
}

// 3.2.4: with no requesting declaration the location stays at `from`, and `at` is still generated.
TEST(GeneratedLocation, anUnknownRequestFallsBackToFrom) {
    GenerationSite site;
    site.via = kViaTemplate;
    GenerationScope scope(site);
    Exception e("m", "CAJETA_ERROR_X", "lib/Box.cajeta", 11, 35);
    EXPECT_EQ(e.getFile(), "lib/Box.cajeta");
    EXPECT_EQ(e.getLine(), 11);
    std::string out = emitted(e);
    EXPECT_TRUE(has(out, "\"file\":\"lib/Box.cajeta\",\"line\":11,\"column\":35")) << out;
    EXPECT_TRUE(has(out, "\"at\":\"generated\"")) << out;
    EXPECT_TRUE(has(out, "\"via\":\"template\"")) << out;
    EXPECT_TRUE(has(out, "\"from\":{\"file\":\"lib/Box.cajeta\",\"line\":11,\"column\":35}")) << out;
}

TEST(GeneratedLocation, aKnownRequestMovesTheLocationAndKeepsFrom) {
    GenerationSite site;
    site.via = kViaTemplate;
    site.request = {"test/Use.cajeta", 16, 9};
    GenerationScope scope(site);
    Exception e("m", "CAJETA_ERROR_X", "lib/Box.cajeta", 11, 35);
    std::string out = emitted(e);
    EXPECT_TRUE(has(out, "\"file\":\"test/Use.cajeta\",\"line\":16,\"column\":9")) << out;
    EXPECT_TRUE(has(out, "\"from\":{\"file\":\"lib/Box.cajeta\",\"line\":11,\"column\":35}")) << out;
}

// A synthesized body has no file of its own, so `from` is the generator's declaration whatever the inner position.
TEST(GeneratedLocation, aSynthesizedBodyAnchorsFromAtTheGenerator) {
    GenerationSite site;
    site.via = synthesizerVia("json");
    site.request = {"test/Pt.cajeta", 10, 8};
    site.anchor = {"<stdlib>/cajeta/codec/json/Json.cajeta", 210, 5};
    site.anchorOnly = true;
    GenerationScope scope(site);
    Exception e("m", "CAJETA_ERROR_X");
    std::string out = emitted(e);
    EXPECT_TRUE(has(out, "\"file\":\"test/Pt.cajeta\",\"line\":10,\"column\":8")) << out;
    EXPECT_TRUE(has(out, "\"via\":\"json-synthesizer\"")) << out;
    EXPECT_TRUE(has(out, from("<stdlib>/cajeta/codec/json/Json.cajeta", 210))) << out;
}

TEST(GeneratedLocation, aPlainMethodScopeMasksAnOuterGeneratedScope) {
    GenerationSite generated;
    generated.via = kViaTemplate;
    generated.request = {"test/Use.cajeta", 16, 9};
    GenerationScope outer(generated);
    GenerationScope inner{GenerationSite{}};
    Exception e("m", "CAJETA_ERROR_X", "test/Other.cajeta", 4, 2);
    EXPECT_EQ(e.getFile(), "test/Other.cajeta");
    EXPECT_TRUE(e.getGenerated().via.empty());
    EXPECT_TRUE(has(emitted(e), "\"at\":\"source\"")) << emitted(e);
}

// 6.3: one trailing clause for generated code, none for source.
TEST(GeneratedLocation, theTextClauseNamesTheGeneratorAndItsSource) {
    GeneratedOrigin g;
    g.via = "json-synthesizer";
    g.fromFile = "<stdlib>/cajeta/codec/json/Json.cajeta";
    g.fromLine = 210;
    EXPECT_EQ(generatedClause(g),
              " (in code generated by json-synthesizer from <stdlib>/cajeta/codec/json/Json.cajeta:210)");
    GeneratedOrigin noFrom;
    noFrom.via = "default-constructor";
    EXPECT_EQ(generatedClause(noFrom), " (in code generated by default-constructor)");
    EXPECT_EQ(generatedClause(GeneratedOrigin{}), "");
}

// 3.2.1: an error inside a class template instantiation points at the line that instantiated it.
TEST(GeneratedLocation, aClassTemplateErrorPointsAtTheInstantiatingLine) {
    Fixture f = run(kBox, "json");
    if (f.root.empty()) GTEST_SKIP() << "no compiler binary";
    EXPECT_TRUE(has(f.out, "CAJETA_ERROR_MEMBER_NOT_FOUND")) << f.out;
    EXPECT_TRUE(has(f.out, at(f, 16))) << f.out;
    EXPECT_TRUE(has(f.out, "\"at\":\"generated\",\"via\":\"template\"")) << f.out;
    EXPECT_TRUE(has(f.out, from(f.file, 11))) << f.out;
}

TEST(GeneratedLocation, theTextFormCarriesTheClauseForATemplate) {
    Fixture f = run(kBox, "text");
    if (f.root.empty()) GTEST_SKIP() << "no compiler binary";
    EXPECT_TRUE(has(f.out, f.file + ":16:")) << f.out;
    EXPECT_TRUE(has(f.out, "(in code generated by template from " + f.file + ":11)")) << f.out;
}

TEST(GeneratedLocation, aSourceErrorHasNoClause) {
    Fixture f = run("package test;\n\npublic final class Use {\n"
                    "    public static int64 f() { return Use.nope(); }\n}\n", "text");
    if (f.root.empty()) GTEST_SKIP() << "no compiler binary";
    EXPECT_TRUE(has(f.out, f.file + ":4:")) << f.out;
    EXPECT_FALSE(has(f.out, "(in code generated by")) << f.out;
}

// 3.2.1, 2.3.1: a field-store error inside an instantiation points at the instantiating line.
TEST(GeneratedLocation, aStoreErrorInATemplateInstantiationPointsAtTheInstantiatingLine) {
    std::string src = kBox;
    src.replace(src.find("this.v #= x;"), 12, "this.v = x; ");
    Fixture f = run(src, "lint");
    if (f.root.empty()) GTEST_SKIP() << "no compiler binary";
    EXPECT_TRUE(has(f.out, "CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE")) << f.out;
    EXPECT_TRUE(has(f.out, at(f, 16))) << f.out;
    EXPECT_TRUE(has(f.out, "\"via\":\"template\"")) << f.out;
    EXPECT_TRUE(has(f.out, from(f.file, 10))) << f.out;
}

// 3.2.1: a generic method's error points at the call that instantiated it, and `from` at the real template line.
TEST(GeneratedLocation, aMethodTemplateErrorPointsAtTheCall) {
    Fixture f = run("package test;\n\npublic class Cell {\n    public int64 n;\n"
                    "    public Cell(int64 v) { this.n = v; }\n}\n\npublic final class Util {\n"
                    "    public static int64 weighOf<T>(T v) {\n        return v.weight();\n    }\n}\n\n"
                    "public final class Use {\n    public static int64 f() {\n"
                    "        Cell c = heap Cell(3);\n        return Util.weighOf<Cell>(c);\n    }\n}\n",
                    "json");
    if (f.root.empty()) GTEST_SKIP() << "no compiler binary";
    EXPECT_TRUE(has(f.out, at(f, 17))) << f.out;
    EXPECT_TRUE(has(f.out, "\"via\":\"template\"")) << f.out;
    EXPECT_TRUE(has(f.out, from(f.file, 10))) << f.out;
}

// 3.2.2: a synthesized codec that fails to compile points at the record that asked for it.
TEST(GeneratedLocation, aJsonCodecErrorPointsAtTheRecord) {
    Fixture f = run("package test;\n\nimport cajeta.codec.json.Json;\n\npublic class Weird {\n"
                    "    public int64 q;\n    public Weird(int64 a, int64 b) { this.q = a + b; }\n}\n\n"
                    "public class Pt {\n    public int64 x;\n    public Weird w;\n}\n\n"
                    "public final class Use {\n    public static int64 f() {\n"
                    "        Pt p #= Json.parse<Pt>(\"{\\\"x\\\":1}\");\n        return p.x;\n    }\n}\n",
                    "json");
    if (f.root.empty()) GTEST_SKIP() << "no compiler binary";
    EXPECT_TRUE(has(f.out, "CAJETA_ERROR_NO_MATCHING_CONSTRUCTOR")) << f.out;
    EXPECT_TRUE(has(f.out, at(f, 10))) << f.out;
    EXPECT_TRUE(has(f.out, "\"via\":\"json-synthesizer\"")) << f.out;
    EXPECT_TRUE(has(f.out, "\"from\":{\"file\":\"<stdlib>/cajeta/codec/json/Json.cajeta\",")) << f.out;
}

TEST(GeneratedLocation, aCsvCodecErrorPointsAtTheRecord) {
    Fixture f = run("package test;\n\nimport cajeta.codec.csv.Csv;\n\npublic class Row {\n"
                    "    public int64 x;\n    public Row(int64 a) { this.x = a; }\n}\n\n"
                    "public final class Use {\n    public static int64 f() {\n"
                    "        int8[] b #= \"x\\n1\\n\".toBytes();\n"
                    "        Row[] rows #= Csv.parse<Row[]>(b, (int64) b.count());\n"
                    "        return rows[0].x;\n    }\n}\n",
                    "json");
    if (f.root.empty()) GTEST_SKIP() << "no compiler binary";
    EXPECT_TRUE(has(f.out, "CAJETA_ERROR_NO_MATCHING_CONSTRUCTOR")) << f.out;
    EXPECT_TRUE(has(f.out, at(f, 5))) << f.out;
    EXPECT_TRUE(has(f.out, "\"via\":\"csv-synthesizer\"")) << f.out;
    EXPECT_TRUE(has(f.out, "\"from\":{\"file\":\"<stdlib>/cajeta/codec/csv/Csv.cajeta\",")) << f.out;
}

// 3.2.3: an error in a generated default constructor points at the class, and `from` at the initializer.
TEST(GeneratedLocation, aDefaultConstructorErrorPointsAtTheClass) {
    Fixture f = run("package test;\n\npublic final class Util {\n"
                    "    public static int64 one() { return 1; }\n}\n\n"
                    "public class Plain {\n    public int64 k = Util.nope();\n}\n\n"
                    "public final class Use {\n    public static int64 f() {\n"
                    "        Plain p = heap Plain();\n        return p.k;\n    }\n}\n",
                    "json");
    if (f.root.empty()) GTEST_SKIP() << "no compiler binary";
    EXPECT_TRUE(has(f.out, at(f, 7))) << f.out;
    EXPECT_TRUE(has(f.out, "\"via\":\"default-constructor\"")) << f.out;
    EXPECT_TRUE(has(f.out, from(f.file, 8))) << f.out;
}
