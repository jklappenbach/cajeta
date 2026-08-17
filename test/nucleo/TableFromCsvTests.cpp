//
// nucleo-frame 16.1.1 (resurrected by table-fit U2) — `Table.fromCsv<R>`:
// CSV text → typed Table<R>. The header binds columns BY NAME to R's fields
// (order-free, extra CSV columns ignored); physicals come from R's schema
// (v1: float64 / int64 / Instant epoch-nanos / Utf8, non-nullable); the
// synthesized schema-descriptor body drives the concrete `DynFrame.fromCsv`
// parser and rebinds through the same positional airlock `importArrow<R>`
// uses. Malformed input fails loud (FrameException), never as silent nulls.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <map>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kPrelude =
    "package test;\n"
    "import cajeta.time.Instant;\n"
    "import cajeta.lang.Utf8;\n"
    "import cajeta.nucleo.frame.Table;\n"
    "import cajeta.nucleo.frame.FrameException;\n"
    "public record Quote {\n"
    "    Instant ts;\n"
    "    float64 price;\n"
    "    Utf8 venue;\n"
    "}\n";

// 2.1.1 — typed columns parsed; header→field mapping is by name and
// order-free (the CSV column order differs from the schema order, and an
// extra column is ignored).
TEST(TableFromCsvTests, typedColumnsParseWithHeaderMapping) {
    std::string src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String csv = \"venue,junk,price,ts\\n\"\n"
        "            + \"NYSE,x,1.5,1000\\n\"\n"
        "            + \"BATS,y,2.5,2000\\n\"\n"
        "            + \"ARCA,z,3.5,3000\\n\";\n"
        "        Table<Quote> t #= Table.fromCsv<Quote>(csv);\n"
        "        if (t.rowCount() != 3) { return 1; }\n"
        "        if (t.price.get(0) != 1.5) { return 2; }\n"
        "        if (t.price.get(2) != 3.5) { return 3; }\n"
        "        if (t.ts.get(1) != 2000) { return 4; }\n"
        "        if (!t.venue.get(0).equals(\"NYSE\")) { return 5; }\n"
        "        if (!t.venue.get(2).equals(\"ARCA\")) { return 6; }\n"
        "        return 42;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 42);
}

// 2.1.2a — a schema column missing from the header fails loud.

// 2.1.2b — a row whose arity disagrees with the header fails loud.

// The table-fit composition: the record lives in its OWN file and fromCsv is
// the template-static body that performs the first Table<R> instantiation —
// the exact shape that used to SIGSEGV (defect A) and then FRAME_SCHEMA-fail
// (defect B) before the materialization fix.

} // namespace
