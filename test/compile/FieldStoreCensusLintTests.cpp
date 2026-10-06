// field-store-ownership plan 1.2.4: under `--lint`, the census instantiates every template once with
// stand-in arguments, so a template nobody instantiates is walked too.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string compilerBinary() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    std::string r = (envRoot && *envRoot) ? envRoot :
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
        CAJETA_SOURCE_ROOT_DEFAULT;
#else
        ".";
#endif
    return r + "/build/src/cajeta";
}

fs::path freshTempDir(const std::string& tag) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path() / ("cajeta_fsc_" + tag + "_" + std::to_string(rng()));
    fs::create_directories(base);
    return base;
}

void writeUnit(const fs::path& root, const std::string& rel, const std::string& text) {
    auto file = root / rel;
    fs::create_directories(file.parent_path());
    std::ofstream(file) << text;
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void setCensus(bool on) {
#ifdef _WIN32
    _putenv_s("CAJETA_FIELD_STORE_CENSUS", on ? "1" : "");
#else
    if (on) setenv("CAJETA_FIELD_STORE_CENSUS", "1", 1);
    else unsetenv("CAJETA_FIELD_STORE_CENSUS");
#endif
}

// Lints `root` and returns everything the compiler printed.
std::string lint(const fs::path& root, bool census) {
    auto dir = freshTempDir("out");
    auto out = dir / "lint.txt";
    std::string cmd = compilerBinary() + " --lint " + root.string() + " --emit-xref="
                    + (dir / "xref.json").string() + " > " + out.string() + " 2>&1";
    setCensus(census);
    (void) std::system(cmd.c_str());
    setCensus(false);
    return slurp(out);
}

fs::path templateProject() {
    auto root = freshTempDir("src");
    writeUnit(root, "test/Cell.cajeta",
        "package test;\n"
        "public class Cell {\n"
        "    public int64 n;\n"
        "    public Cell(int64 v) { this.n = v; }\n"
        "}\n");
    writeUnit(root, "test/Box.cajeta",
        "package test;\n"
        "public final class Box<T> {\n"
        "    public T v;\n"
        "    public Box() { }\n"
        "    public void keepPlain(T p) { this.v = p; }\n"
        "    public void keepSharp(T p) { this.v #= p; }\n"
        "}\n");
    writeUnit(root, "test/NumBox.cajeta",
        "package test;\n"
        "public final class NumBox<T extends Numeric> {\n"
        "    public T v;\n"
        "    public NumBox() { }\n"
        "    public void keep(T p) { this.v = p; }\n"
        "}\n");
    writeUnit(root, "test/Sized.cajeta",
        "package test;\n"
        "public final class Sized<uint32 N> {\n"
        "    public Cell c;\n"
        "    public Sized() { }\n"
        "    public void keep(Cell p) { this.c = p; }\n"
        "}\n");
    writeUnit(root, "test/Thirds.cajeta",
        "package test;\n"
        "@Requires(N % 3 == 0)\n"
        "public final class Thirds<uint32 N> {\n"
        "    public Cell c;\n"
        "    public Thirds() { }\n"
        "    public void keep(Cell p) { this.c = p; }\n"
        "}\n");
    return root;
}

bool has(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

}  // namespace

TEST(FieldStoreCensusLintTests, templatesNobodyInstantiatesAreWalked) {
    if (!fs::exists(compilerBinary())) GTEST_SKIP() << "no compiler binary";
    std::string out = lint(templateProject(), true);

    EXPECT_TRUE(has(out, "[field-store-template] test.Box<cajeta.lang.Object>\n")) << out;
    EXPECT_TRUE(has(out, "[field-store] test.Box.keepPlain:5 into=field op== src=formal-plain name=p type=cajeta.lang.Object")) << out;
    EXPECT_TRUE(has(out, "[field-store] test.Box.keepSharp:6 into=field op=#= src=formal-plain name=p")) << out;
    EXPECT_TRUE(has(out, "CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE test.Box.keepPlain")) << out;
    EXPECT_FALSE(has(out, "test.Box.keepSharp\n")) << out;

    EXPECT_TRUE(has(out, "[field-store-template] test.Sized<16>\n")) << out;
    EXPECT_TRUE(has(out, "CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE test.Sized.keep")) << out;

    EXPECT_TRUE(has(out, "[field-store-template] test.NumBox<int64>\n")) << out;
    EXPECT_FALSE(has(out, "[field-store] test.NumBox.keep")) << out;
}

TEST(FieldStoreCensusLintTests, aTemplateThatRefusesItsStandInIsListedWithTheReason) {
    if (!fs::exists(compilerBinary())) GTEST_SKIP() << "no compiler binary";
    std::string out = lint(templateProject(), true);
    EXPECT_TRUE(has(out, "[field-store-template] test.Thirds<16> skipped: Thirds<16> is refused")) << out;
    EXPECT_FALSE(has(out, "test.Thirds.keep")) << out;
}

TEST(FieldStoreCensusLintTests, aLintWithoutTheCensusInstantiatesNothing) {
    if (!fs::exists(compilerBinary())) GTEST_SKIP() << "no compiler binary";
    std::string out = lint(templateProject(), false);
    EXPECT_FALSE(has(out, "[field-store")) << out;
    EXPECT_FALSE(has(out, "KEEP_NEEDS_SHARP_STORE")) << out;
}
