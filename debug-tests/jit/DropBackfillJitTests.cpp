//
// JIT drop-thunk backfill (jit-drop-backfill spec §3, §4).
//
// Drop thunks are synthesized lazily into the owning type's module when a
// consumer's codegen drops a value of the type; declarations whose synthesis
// never fired dangle. buildJit historically ran no backfill, so LLJIT
// initialize failed with `Symbols not found: [__cajeta[_stack]_<type>_drop,
// ...]` on any program big enough to dangle one — samples/tour was the live
// reproduction (run-config-ergonomics 7.3.2), while every small debug-tests
// fixture materialized fine. These tests pin the gap at suite size: a program
// built to drop generic value instantiations only through indirect stdlib
// paths, mirroring the tour signature (Optional<...>, nested Pair<...>).
//
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "cajeta/jit/CajetaJitHost.h"

namespace fs = std::filesystem;
using cajeta::jit::JitRunOptions;
using cajeta::jit::runJit;

namespace {

fs::path writeProgram(const std::string& pkgRelPath,
                      const std::string& fileName,
                      const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    fs::path root = fs::temp_directory_path()
                  / ("cajeta_dbgtest_" + std::to_string(rng()));
    fs::path dir = root / pkgRelPath;
    fs::create_directories(dir);
    std::ofstream out(dir / fileName);
    out << source;
    out.close();
    return root;
}

struct TempProgram {
    fs::path root;
    ~TempProgram() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

} // namespace

namespace {

void writeFile(const fs::path& p, const std::string& src) {
    std::ofstream out(p);
    out << src;
}

// The samples/tour failure, distilled to five files (bisected 2026-07-20;
// see agents/jit-drop-backfill-plan.md 2.1.1 notes). A stack drop thunk is
// synthesized linkonce_odr into the OWNING class's module (here: Shape),
// while the consumer (InheritanceDemo — a base-typed local holding a
// stack-allocated derived instance) holds an extern declaration.
// llvm::Linker lazy-links linkonce_odr, so the definition can be discarded
// during the donor merge, and LLJIT initialize then fails with
// `Symbols not found: [__cajeta_stack_tour_lang_Shape_drop]`.
//
// The String concat + println in execute() are LOAD-BEARING: variants
// without stdlib String involvement in the consumer module do not dangle
// (flat-package / no-println variants stayed green through bisection).
// Nothing calls execute() — the failure is a compile/merge property.
fs::path writeTourShapeSubset() {
    fs::path root = writeProgram("tour", "Entry.cajeta",
        "package tour;\n"
        "public class Entry {\n"
        "    public static int32 main() {\n"
        "        return 3;\n"
        "    }\n"
        "}\n");
    fs::create_directories(root / "tour" / "lang");
    writeFile(root / "tour" / "lang" / "Shape.cajeta",
        "package tour.lang;\n"
        "public class Shape {\n"
        "    public int32 area() {\n"
        "        return 0;\n"
        "    }\n"
        "}\n");
    writeFile(root / "tour" / "lang" / "Square.cajeta",
        "package tour.lang;\n"
        "public class Square extends Shape {\n"
        "    int32 side;\n"
        "    public Square(int32 s) {\n"
        "        this.side = s;\n"
        "    }\n"
        "    public int32 area() {\n"
        "        return this.side * this.side;\n"
        "    }\n"
        "}\n");
    writeFile(root / "tour" / "lang" / "Circle.cajeta",
        "package tour.lang;\n"
        "public class Circle extends Shape {\n"
        "    int32 r;\n"
        "    public Circle(int32 r) {\n"
        "        this.r = r;\n"
        "    }\n"
        "    public int32 area() {\n"
        "        return 3 * this.r * this.r;\n"
        "    }\n"
        "}\n");
    writeFile(root / "tour" / "lang" / "InheritanceDemo.cajeta",
        "package tour.lang;\n"
        "public class InheritanceDemo {\n"
        "    public void execute() {\n"
        "        Shape sq = stack Square(5);\n"
        "        Shape ci = stack Circle(3);\n"
        "        System.stdout.println(\"  Square(5).area() = \" + sq.area());\n"
        "        System.stdout.println(\"  Circle(3).area() = \" + ci.area());\n"
        "    }\n"
        "}\n");
    return root;
}

} // namespace

// 2.1.1 — cross-module stack-drop thunks must survive the JIT module merge.
// Pre-fix: `Symbols not found: [__cajeta_stack_tour_lang_Shape_drop]` at
// LLJIT initialize.
TEST(DropBackfillJit, CrossModuleDropThunkSurvivesMerge) {
    TempProgram p{writeTourShapeSubset()};
    JitRunOptions opts;
    opts.sourceRoot = p.root.string();
    opts.entryMethod = "tour.Entry.main";
    EXPECT_EQ(runJit(opts), 3);
}

// 2.1.2 — spec 3.2.3: a trivial program's behavior is unchanged by the
// backfill (the pass must be a no-op when nothing dangles).
TEST(DropBackfillJit, TrivialProgramUnchanged) {
    TempProgram p{writeProgram("demo", "Tiny.cajeta",
        "package demo;\n"
        "public class Tiny {\n"
        "    public static int32 main() {\n"
        "        return 11;\n"
        "    }\n"
        "}\n")};
    JitRunOptions opts;
    opts.sourceRoot = p.root.string();
    opts.entryMethod = "demo.Tiny.main";
    EXPECT_EQ(runJit(opts), 11);
}
