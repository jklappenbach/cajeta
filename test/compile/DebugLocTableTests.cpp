// external-debug Unit 3 — the location table, embedded in the binary.
//
// DbgLocTable (loc_id -> file, line, col, function) is a compiler-PROCESS
// global. `cajeta dap` reads it only because the DAP compiles and runs in one
// process; an external debugger attached to a built binary has no compiler, so
// it could not map a safepoint to a source line, nor a source line to a
// breakpoint (spec external-debug §3).
//
// Under --debug-info=full, codegen serializes the table into the module and
// registers it with the runtime through a global ctor. Two halves, tested where
// each actually lives:
//
//   - The runtime accessors run in-process, against a table this test registers
//     itself. (The test binary links its own native copy of the runtime, so
//     these calls hit the host's statics.)
//   - The embedding is checked on the AOT artifact — the IR the compiler wrote —
//     because "with no compiler present" is the whole claim. Reaching the
//     accessors through the JIT would prove nothing: the ctor there writes the
//     JIT'd module's copy of the runtime statics, not the host's.
//
// Pins:
//   3.1.1  Round-trip: every id resolves to what the compiler recorded.
//   3.1.2  ids_for_line returns exactly the ids on that (file, line).
//   3.1.3  The accessors survive DCE / --gc-sections (nothing calls them).
//   3.1.4  A `line`/`off` build embeds no table; the accessors report empty
//          rather than crashing.
//   3.1.5  Same source from two build roots -> byte-identical table.

#include "gtest/gtest.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <regex>
#include <string>
#include <vector>

#ifdef _WIN32
#  define CAJETA_DEVNULL "NUL"
#else
#  define CAJETA_DEVNULL "/dev/null"
#endif

// The runtime's C ABI (cajeta_rt_core.c), linked natively into this binary.
extern "C" {
    typedef struct {
        const char* file;
        int32_t     line;
        int32_t     col;
        const char* func;
    } CajetaDbgLocEntry;

    void        __cajeta_dbg_register_loc_table(const CajetaDbgLocEntry*, int32_t);
    int32_t     __cajeta_dbg_loc_count(void);
    const char* __cajeta_dbg_loc_file(int32_t);
    int32_t     __cajeta_dbg_loc_line(int32_t);
    int32_t     __cajeta_dbg_loc_col(int32_t);
    const char* __cajeta_dbg_loc_func(int32_t);
    int32_t     __cajeta_dbg_ids_for_line(const char*, int32_t, int32_t*, int32_t);
}

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

    TmpProject makeTmpProject(const std::string& tag) {
        static std::mt19937_64 rng(std::random_device{}());
        auto base = fs::temp_directory_path()
                  / ("cajeta_loctab_" + tag + "_" + std::to_string(rng()));
        fs::create_directories(base / "src" / "demo");
        fs::create_directories(base / "build");
        std::ofstream out(base / "src" / "demo" / "Hello.cajeta");
        out << "package demo;\n"
            << "public final class Hello {\n"
            << "    public static int32 run() {\n"
            << "        int32 n = 7;\n"
            << "        n = n + 1;\n"
            << "        return n;\n"
            << "    }\n"
            << "}\n";
        return TmpProject{base, base / "src", base / "build"};
    }

    bool compileTo(const TmpProject& p, const std::string& emit,
                   const std::string& level) {
        std::string cmd = compilerPath()
            + " --debug-info=" + level + " --emit=" + emit
            + " demo.Hello.run " + p.sourceRoot.string() + " "
            + p.buildRoot.string() + " > " CAJETA_DEVNULL " 2>&1";
        return std::system(cmd.c_str()) == 0;
    }

    std::string readStdlibIr(const TmpProject& p) {
        std::ifstream f(p.buildRoot / "cajeta.runtime.__stdlib__.ll");
        return std::string(std::istreambuf_iterator<char>(f),
                           std::istreambuf_iterator<char>());
    }

    // Every .ll the compile wrote, concatenated. loc_ids are global across
    // modules — the user module's safepoints share the stdlib's numbering — so
    // any count over the ids has to span all of them.
    std::string readAllIr(const TmpProject& p) {
        std::string all;
        for (const auto& e : fs::recursive_directory_iterator(p.buildRoot)) {
            if (!e.is_regular_file() || e.path().extension() != ".ll") continue;
            std::ifstream f(e.path());
            all.append(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
        }
        return all;
    }

    // Is `fn` DEFINED (not merely declared) in this IR? Signature text varies —
    // LLVM decorates definitions with attributes like `range(i32 0, 8)` between
    // `define` and the return type — so match the name at the definition site.
    bool definesFunction(const std::string& ir, const std::string& fn) {
        std::regex re("define[^\\n]*@" + fn + "\\(");
        return std::regex_search(ir, re);
    }

    // The table's serialized entries as they appear in the IR:
    // `{ ptr @.cajeta.dbg.str.N, i32 <line>, i32 <col>, ptr @.cajeta.dbg.str.M }`
    std::vector<std::string> tableEntries(const std::string& ir) {
        std::vector<std::string> out;
        auto open = ir.find("@__cajeta.dbg.loctable = private constant");
        if (open == std::string::npos) return out;
        std::string decl = ir.substr(open, ir.find('\n', open) - open);
        std::regex re(R"(\{ ptr @[\w.]+, i32 \d+, i32 \d+, ptr @[\w.]+ \})");
        for (std::sregex_iterator it(decl.begin(), decl.end(), re), last;
             it != last; ++it) {
            out.push_back(it->str());
        }
        return out;
    }

    size_t countOf(const std::string& hay, const std::string& needle) {
        size_t n = 0, pos = 0;
        while ((pos = hay.find(needle, pos)) != std::string::npos) { ++n; ++pos; }
        return n;
    }

    // A fixed table for the accessor tests. Static storage — the runtime keeps
    // the pointer, it does not copy.
    const CajetaDbgLocEntry kTable[] = {
        {"demo/Hello.cajeta",       4,  9, "demo.Hello::run"},
        {"demo/Hello.cajeta",       5,  9, "demo.Hello::run"},
        {"demo/Hello.cajeta",       5, 20, "demo.Hello::run"},  // 2 stmts, 1 line
        {"cajeta/lang/Guid.cajeta", 4,  9, "cajeta.lang.Guid::parse"},
    };

    struct RegisteredTable {
        RegisteredTable()  { __cajeta_dbg_register_loc_table(kTable, 4); }
        ~RegisteredTable() { __cajeta_dbg_register_loc_table(nullptr, 0); }
    };

} // namespace

// ---- the runtime accessors -------------------------------------------

// 3.1.1 — round-trip: what the compiler wrote is what the debugger reads.
TEST(DebugLocTable, accessorsRoundTripEveryId) {
    RegisteredTable reg;
    ASSERT_EQ(__cajeta_dbg_loc_count(), 4);

    for (int32_t id = 0; id < 4; ++id) {
        EXPECT_STREQ(__cajeta_dbg_loc_file(id), kTable[id].file) << "id " << id;
        EXPECT_EQ(__cajeta_dbg_loc_line(id), kTable[id].line)    << "id " << id;
        EXPECT_EQ(__cajeta_dbg_loc_col(id), kTable[id].col)      << "id " << id;
        EXPECT_STREQ(__cajeta_dbg_loc_func(id), kTable[id].func) << "id " << id;
    }
}

// 3.1.2 — the reverse direction, which is what arms a breakpoint. Ids are one
// per statement and NOT deduplicated, so two statements on one line yield two
// ids and a line breakpoint must arm both.
TEST(DebugLocTable, idsForLineReturnsEveryIdOnThatLine) {
    RegisteredTable reg;

    int32_t ids[8];
    ASSERT_EQ(__cajeta_dbg_ids_for_line("demo/Hello.cajeta", 5, ids, 8), 2);
    EXPECT_EQ(ids[0], 1);
    EXPECT_EQ(ids[1], 2);

    ASSERT_EQ(__cajeta_dbg_ids_for_line("demo/Hello.cajeta", 4, ids, 8), 1);
    EXPECT_EQ(ids[0], 0);

    // The same line number in a DIFFERENT file must not bleed across.
    ASSERT_EQ(__cajeta_dbg_ids_for_line("cajeta/lang/Guid.cajeta", 4, ids, 8), 1);
    EXPECT_EQ(ids[0], 3);

    EXPECT_EQ(__cajeta_dbg_ids_for_line("demo/Hello.cajeta", 999, nullptr, 0), 0);
}

// A debugger types `cjbreak Hello.cajeta:5`, not the table's stored relative
// path. The basename resolves — but only on a path boundary.
TEST(DebugLocTable, idsForLineAcceptsABasenameOnAPathBoundary) {
    RegisteredTable reg;

    EXPECT_EQ(__cajeta_dbg_ids_for_line("Hello.cajeta", 5, nullptr, 0), 2);
    EXPECT_EQ(__cajeta_dbg_ids_for_line("Guid.cajeta", 4, nullptr, 0), 1);
    // A suffix that is not a whole path component must NOT match.
    EXPECT_EQ(__cajeta_dbg_ids_for_line("ello.cajeta", 5, nullptr, 0), 0);
    EXPECT_EQ(__cajeta_dbg_ids_for_line("MyHello.cajeta", 5, nullptr, 0), 0);
}

// `max` bounds the write, but the return is the TRUE count — so a caller can
// size its buffer from a counting call and never overrun.
TEST(DebugLocTable, idsForLineHonorsTheOutputCap) {
    RegisteredTable reg;

    int32_t one[1] = {-1};
    EXPECT_EQ(__cajeta_dbg_ids_for_line("demo/Hello.cajeta", 5, one, 1), 2);
    EXPECT_EQ(one[0], 1);
    EXPECT_EQ(__cajeta_dbg_ids_for_line("demo/Hello.cajeta", 5, nullptr, 0), 2);
}

// 3.1.4 — an `off`/`line` binary registers nothing. Every accessor answers
// benignly rather than dereferencing null.
TEST(DebugLocTable, emptyTableIsBenign) {
    __cajeta_dbg_register_loc_table(nullptr, 0);

    EXPECT_EQ(__cajeta_dbg_loc_count(), 0);
    EXPECT_STREQ(__cajeta_dbg_loc_file(0), "");
    EXPECT_EQ(__cajeta_dbg_loc_line(0), 0);
    EXPECT_EQ(__cajeta_dbg_loc_col(0), 0);
    EXPECT_STREQ(__cajeta_dbg_loc_func(0), "");
    EXPECT_EQ(__cajeta_dbg_ids_for_line("demo/Hello.cajeta", 5, nullptr, 0), 0);
}

// Out-of-range ids are benign too — a stale debugger script must not take the
// inferior down with it.
TEST(DebugLocTable, outOfRangeIdsAreBenign) {
    RegisteredTable reg;

    for (int32_t bad : {-1, 4, 1 << 20}) {
        EXPECT_STREQ(__cajeta_dbg_loc_file(bad), "") << bad;
        EXPECT_EQ(__cajeta_dbg_loc_line(bad), 0)     << bad;
        EXPECT_EQ(__cajeta_dbg_loc_col(bad), 0)      << bad;
        EXPECT_STREQ(__cajeta_dbg_loc_func(bad), "") << bad;
    }
}

// ---- the embedding (AOT artifact) ------------------------------------

// The table is emitted, sized to match the safepoints, and handed to the runtime
// by a startup ctor.
TEST(DebugLocTable, fullBuildEmbedsTheTableAndRegistersIt) {
    auto p = makeTmpProject("embed");
    ASSERT_TRUE(compileTo(p, "ir", "full"));
    auto ir = readStdlibIr(p);
    ASSERT_FALSE(ir.empty());

    auto entries = tableEntries(ir);
    EXPECT_GT(entries.size(), 0u);
    // One entry per safepoint, counted across EVERY module: the ids codegen
    // handed out are the table's indices, and they are global — the user
    // module's safepoints continue the stdlib's numbering.
    EXPECT_EQ(entries.size(),
              countOf(readAllIr(p), "call void @__cajeta_dbg_safepoint("));
    EXPECT_NE(ir.find("call void @__cajeta_dbg_register_loc_table("),
              std::string::npos);
    EXPECT_NE(ir.find("@llvm.global_ctors"), std::string::npos);

    fs::remove_all(p.base);
}

// 3.1.3 — the accessors are called by NOBODY. Without `used`/`retain` both DCE
// and --gc-sections drop them, which is exactly how __cajeta_print_stack was
// lost on its first cut. They must be in the runtime's llvm.used and reach the
// emitted module.
TEST(DebugLocTable, accessorsSurviveDeadCodeElimination) {
    auto p = makeTmpProject("retain");
    ASSERT_TRUE(compileTo(p, "ir", "full"));
    auto ir = readStdlibIr(p);
    ASSERT_FALSE(ir.empty());

    for (const char* fn : {"__cajeta_dbg_loc_count", "__cajeta_dbg_loc_file",
                           "__cajeta_dbg_loc_line", "__cajeta_dbg_loc_col",
                           "__cajeta_dbg_loc_func", "__cajeta_dbg_ids_for_line"}) {
        EXPECT_TRUE(definesFunction(ir, fn))
            << fn << " was dropped from the module";
    }
    // The retain anchor itself.
    auto used = ir.find("@llvm.used");
    ASSERT_NE(used, std::string::npos);
    std::string usedDecl = ir.substr(used, ir.find('\n', used) - used);
    EXPECT_NE(usedDecl.find("__cajeta_dbg_loc_count"), std::string::npos);
    EXPECT_NE(usedDecl.find("__cajeta_dbg_ids_for_line"), std::string::npos);

    fs::remove_all(p.base);
}

// 3.1.4 (emit side) — nothing is embedded below `full`.
TEST(DebugLocTable, lineAndOffBuildsEmbedNoTable) {
    for (const char* level : {"line", "off"}) {
        auto p = makeTmpProject(std::string("no-table-") + level);
        ASSERT_TRUE(compileTo(p, "ir", level)) << level;
        auto ir = readStdlibIr(p);
        ASSERT_FALSE(ir.empty()) << level;

        EXPECT_EQ(ir.find("@__cajeta.dbg.loctable"), std::string::npos) << level;
        EXPECT_EQ(ir.find("call void @__cajeta_dbg_register_loc_table("),
                  std::string::npos) << level;
        fs::remove_all(p.base);
    }
}

// 3.1.5 — the table's file names are the remapped, build-root-independent form,
// so two roots produce byte-identical tables. They used to be
// module->getSourcePath(): the raw ABSOLUTE path.
TEST(DebugLocTable, sameSourceFromTwoRootsYieldsIdenticalTables) {
    auto a = makeTmpProject("rootA");
    auto b = makeTmpProject("rootB");
    ASSERT_TRUE(compileTo(a, "ir", "full"));
    ASSERT_TRUE(compileTo(b, "ir", "full"));

    auto irA = readStdlibIr(a);
    auto ea = tableEntries(irA);
    auto eb = tableEntries(readStdlibIr(b));
    ASSERT_GT(ea.size(), 0u);
    EXPECT_EQ(ea, eb) << "the loc table is not build-root independent";

    EXPECT_EQ(irA.find(a.base.string()), std::string::npos)
        << "build root leaked into the embedded strings";

    fs::remove_all(a.base);
    fs::remove_all(b.base);
}
