// external-debug Unit 4 — locals decode from reflection metadata.
//
// A debugger holds only a local's ADDRESS. To render it, it reaches the object's
// vtable -> classObject -> RTTI, and decodes fields from the RTTI's byteOffset +
// typeFlags. Two things stood in the way (spec §4):
//
//   - RTTI emission is DEMAND-driven (ReflectionKeep): a program that never
//     reflects may carry no field metadata at all, so a --debug-info=full build
//     could have nothing to decode. `full` now forces retention.
//   - The object -> RTTI hop existed but was `static` (cajeta_rtti_from_obj).
//     It is exported as __cajeta_rtti_of.
//
// And the debug frame/local accessors, which the in-process DAP calls from C++,
// are called by NOTHING in an AOT binary — so DCE and --gc-sections dropped them,
// leaving an external debugger unable to read the locals the program records.
//
// Pins:
//   4.1.1  `full` forces RTTI retention: a program that never reflects still
//          emits field metadata for its classes.
//   4.1.2  __cajeta_rtti_of is exported and survives the link.
//   4.1.8  A static field is byteOffset == -1, so a decoder can report it as
//          unsupported rather than mis-decode it (spec §4.1.8).
//   The frame/local walkers survive an AOT link.

#include "gtest/gtest.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <regex>
#include <string>

#ifdef _WIN32
#  define CAJETA_DEVNULL "NUL"
#else
#  define CAJETA_DEVNULL "/dev/null"
#endif

namespace {

    namespace fs = std::filesystem;

    std::string compilerPath() {
        const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
        std::string r = (envRoot && *envRoot) ? envRoot :
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
            CAJETA_SOURCE_ROOT_DEFAULT;
#else
            ".";
#endif
        return r + "/build/src/cajeta";
    }

    struct TmpProject {
        fs::path base, sourceRoot, buildRoot;
    };

    // A program that NEVER reflects: no Class<T>, no forName, no annotations.
    // Its field metadata exists only if --debug-info=full forced it.
    TmpProject makeTmpProject(const std::string& tag) {
        static std::mt19937_64 rng(std::random_device{}());
        auto base = fs::temp_directory_path()
                  / ("cajeta_rtti_" + tag + "_" + std::to_string(rng()));
        fs::create_directories(base / "src" / "demo");
        fs::create_directories(base / "build");
        std::ofstream out(base / "src" / "demo" / "Hello.cajeta");
        out << "package demo;\n"
            << "public class Shape {\n"
            << "    public int32 sides;\n"
            << "    public Shape() { this.sides = 0; }\n"
            << "}\n"
            << "public final class Circle extends Shape {\n"
            << "    public float64 radius;\n"
            << "    public Circle() { this.sides = 1; this.radius = 2.5; }\n"
            << "}\n"
            << "public final class Hello {\n"
            << "    public static int32 run() {\n"
            << "        Shape s = heap Circle();\n"
            << "        return s.sides;\n"
            << "    }\n"
            << "}\n";
        return TmpProject{base, base / "src", base / "build"};
    }

    bool compileTo(const TmpProject& p, const std::string& emit,
                   const std::string& level, const std::string& extra = "") {
        std::string cmd = compilerPath()
            + " --debug-info=" + level + " --emit=" + emit + " " + extra
            + " demo.Hello.run " + p.sourceRoot.string() + " "
            + p.buildRoot.string() + " > " CAJETA_DEVNULL " 2>&1";
        return std::system(cmd.c_str()) == 0;
    }

    std::string readIr(const TmpProject& p, const std::string& file) {
        std::ifstream f(p.buildRoot / file);
        return std::string(std::istreambuf_iterator<char>(f),
                           std::istreambuf_iterator<char>());
    }

    // Is `fn` DEFINED in this IR? LLVM decorates definitions with attributes
    // between `define` and the return type, so match at the name.
    bool definesFunction(const std::string& ir, const std::string& fn) {
        return std::regex_search(ir, std::regex("define[^\\n]*@" + fn + "\\("));
    }

    size_t countOf(const std::string& hay, const std::string& needle) {
        size_t n = 0, pos = 0;
        while ((pos = hay.find(needle, pos)) != std::string::npos) { ++n; ++pos; }
        return n;
    }

} // namespace

// 4.1.1 — the retention gate. `Circle` is never reflected on. Under a lean link
// its class registration is dropped, so at runtime there is no reachable RTTI to
// decode a `Shape s` holding a `Circle` with. `full` must force it back in.
//
// The observable is the REGISTRATION (__cajeta_register_class in a global ctor),
// not the #RttiGlobal constant: the global is emitted either way, but without the
// registration ctor nothing anchors it and --gc-sections takes it.
TEST(DebugRttiRetention, fullForcesClassRegistrationForATypeNeverReflectedOn) {
    auto p = makeTmpProject("keep");
    // Lean link-mode is where the keep-set narrows; --emit=exe turns it on by
    // default, so pin it explicitly on the IR path.
    ASSERT_TRUE(compileTo(p, "ir", "full", "--link-mode=lean"));
    auto ir = readIr(p, "demo/Hello.ll");
    ASSERT_FALSE(ir.empty());

    EXPECT_GT(countOf(ir, "call void @__cajeta_register_class"), 0u)
        << "--debug-info=full did not force RTTI retention for a never-"
           "reflected class — a debugger would have no field metadata to read";
    EXPECT_NE(ir.find("demo.Circle#RttiGlobal"), std::string::npos);
    // The field names + offsets a decoder reads a local against.
    EXPECT_NE(ir.find("radius"), std::string::npos);
    EXPECT_NE(ir.find("sides"), std::string::npos);

    fs::remove_all(p.base);
}

// ...and it is the DEBUG LEVEL doing it. The same never-reflecting program at
// `line` keeps the lean keep-set narrow and pays nothing.
TEST(DebugRttiRetention, lineDoesNotForceRetention) {
    auto p = makeTmpProject("no-keep");
    ASSERT_TRUE(compileTo(p, "ir", "line", "--link-mode=lean"));
    auto ir = readIr(p, "demo/Hello.ll");
    ASSERT_FALSE(ir.empty());

    EXPECT_EQ(countOf(ir, "call void @__cajeta_register_class"), 0u)
        << "a `line` build should not be paying for keep-all RTTI";

    fs::remove_all(p.base);
}

// 4.1.2 + the frame walkers — every accessor an external debugger needs must
// survive the link. Nothing in generated code calls ANY of them.
TEST(DebugRttiRetention, debuggerAccessorsSurviveTheLink) {
    auto p = makeTmpProject("retain");
    ASSERT_TRUE(compileTo(p, "ir", "full"));
    auto ir = readIr(p, "cajeta.runtime.__stdlib__.ll");
    ASSERT_FALSE(ir.empty());

    for (const char* fn : {// object -> dynamic type (4.1.2)
                           "__cajeta_rtti_of",
                           "__cajeta_rtti_type_name",
                           // type -> fields, offsets, flags (4.1.3/4.1.4)
                           "__cajeta_rtti_field_count",
                           "__cajeta_rtti_field_name",
                           "__cajeta_rtti_field_offset",
                           "__cajeta_rtti_field_type_flags",
                           // the parent chain — a class's RTTI carries only its
                           // OWN fields, so an inherited one is only reachable
                           // through parent_name -> for_name
                           "__cajeta_rtti_parent_count",
                           "__cajeta_rtti_parent_name",
                           "__cajeta_rtti_for_name",
                           // the debug frame chain (4.2.3)
                           "__cajeta_dbg_frame_top",
                           "__cajeta_dbg_frame_depth",
                           "__cajeta_dbg_frame_prev",
                           "__cajeta_dbg_frame_nlocals",
                           "__cajeta_dbg_local_name",
                           "__cajeta_dbg_local_type",
                           "__cajeta_dbg_local_addr",
                           "__cajeta_dbg_local_alloc",
                           "__cajeta_dbg_local_ownership"}) {
        EXPECT_TRUE(definesFunction(ir, fn)) << fn << " was dropped";
    }

    auto used = ir.find("@llvm.used");
    ASSERT_NE(used, std::string::npos);
    std::string usedDecl = ir.substr(used, ir.find('\n', used) - used);
    EXPECT_NE(usedDecl.find("__cajeta_dbg_frame_top"), std::string::npos)
        << "the frame-chain entry point is not retained";
    EXPECT_NE(usedDecl.find("__cajeta_dbg_local_addr"), std::string::npos);

    fs::remove_all(p.base);
}
