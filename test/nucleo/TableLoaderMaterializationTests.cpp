//
// Loader materialization for synthesized generics (synth-class-loader-
// materialization spec). Two same-family gaps, both "the name is bound but
// the class isn't loaded when synthesis needs it":
//
//  A. The FIRST `Table<R>` instantiation inside a template-static method
//     body: member-synthesis imports (`DynCol`, …) were only name-bound by
//     `injectImportIfUnbound`, never materialized, so the synthesized
//     `fromColumns` ctor's `DynCol[]` local resolved to a null element type
//     — a compiler SIGSEGV. (This is what blocked nucleo-frame U16's
//     `Table.fromCsv<R>`.)
//
//  B. A record declared in a SEPARATE file from the first `Table<R>` use:
//     the schema synthesizer read the record flag before the record's unit
//     was materialized, throwing a spurious CAJETA_ERROR_FRAME_SCHEMA
//     "'test.Rec' is not a record". Same-file always worked (and the
//     JIT-prelude suites compile one string, which is why they never saw it).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <map>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// --- A: first instantiation inside a template-static body ------------------


// --- B: cross-file record --------------------------------------------------


// Ordering probe: identical shape, but the USING class sorts AFTER the record
// ("test.Zuser" > "test.Rec" in the harness's std::map file order). If this
// passes while crossFileRecordSchemaMaterializes fails, the defect is purely
// parse/walk file order — the record's declaration walk hasn't run when the
// earlier file's body walk instantiates Table<Rec>.

// Nested-materialize prototype hazard (the ml v0.2.0 CI failure): the
// consumer sorts FIRST and declares an EXPLICIT no-arg ctor; its walk hits
// Table<Rec3> mid-body, the nested materialize compile runs, and any
// prototype sweep inside that nested compile (drainLazyStdlib's tail) used
// to prototype the still-BODYLESS consumer — fabricating its auto-default
// ctor, which collided with the declared one when the walk resumed
// (CAJETA_ERROR_DUPLICATE_CONSTRUCTOR). The declWalkInFlight marker keeps
// mid-walk classes out of every sweep.

// Negative control: materializing cross-file types must NOT soften the schema
// contract — a cross-file NON-record argument still fails loudly.
TEST(TableLoaderMaterializationTests, crossFileNonRecordStillRejected) {
    std::map<std::string, std::string> sources;
    sources["test.NotRec"] =
        "package test;\n"
        "public class NotRec {\n"
        "    public float64 v;\n"
        "    public NotRec() { this.v = 0.0; return; }\n"
        "}\n";
    sources["test.D"] =
        "package test;\n"
        "import cajeta.nucleo.frame.Table;\n"
        "import cajeta.nucleo.column.Column;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float64[] pv = heap float64[1];\n"
        "        pv[0] = 1.0;\n"
        "        Table<NotRec> t = heap Table<NotRec>(Column.of<float64>(pv));\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    try {
        auto jit = CajetaJit::compile(sources, "test.D");
        FAIL() << "Table<NotRec> with a cross-file non-record compiled";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_FRAME_SCHEMA")
            << "wrong error: " << e.getMessage();
    }
}

// --- A+B combined: the fromCsv composition ---------------------------------


} // namespace
