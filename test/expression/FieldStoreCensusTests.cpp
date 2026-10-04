// field-store-ownership Unit 1 (plan 1.1.2): the census lists one line per
// field or slot store of a name, with its source kind.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <set>
#include <string>

#include "cajeta/ownership/FieldStoreCensus.h"
#include "cajeta/type/Scope.h"

using cajeta_test::CajetaJit;
using cajeta::ownership::FieldStoreCensus;

namespace {

const char* kSrc =
    "package test;\n"
    "public class Cell {\n"
    "    public int64 n;\n"
    "    public Cell(int64 v) { this.n = v; }\n"
    "}\n"
    "public class Holder {\n"
    "    public Cell c;\n"
    "    public Holder() { }\n"
    "}\n"
    "public final class Keep {\n"
    "    public Cell c;\n"
    "    public Cell[] items;\n"
    "    public int64 n;\n"
    "    static Cell s;\n"
    "    public Keep() { this.items = heap Cell[2]; }\n"
    "    public void plainFormal(Cell p) { this.c = p; }\n"
    "    public void sharpFormal(#Cell p) { this.c #= p; }\n"
    "    public void legacy(#Cell p) { this.c = #p; }\n"
    "    public void heapLocal() { Cell l = heap Cell(1L); this.c #= l; }\n"
    "    public void aliasLocal(Cell p) { Cell l = p; this.c = l; }\n"
    "    public void interior(Holder h) { this.c = h.c; }\n"
    "    public void slot(int32 i, Cell p) { this.items[i] #= p; }\n"
    "    public void toStatic(Cell p) { s #= p; }\n"
    "    public void nested(Holder h, Cell p) { h.c #= p; }\n"
    "    public void producer() { this.c = heap Cell(2L); }\n"
    "    public void primitive(int64 v) { this.n = v; }\n"
    "}\n"
    "public final class D {\n"
    "    public static int32 run() { return 0; }\n"
    "}\n";

std::string key(const cajeta::ownership::FieldStoreRecord& r) {
    return r.methodName + " " + r.target + " " + r.op + " " + r.source + " " + r.name;
}

}  // namespace

TEST(FieldStoreCensusTests, oneLinePerStoreKind) {
    FieldStoreCensus::clear();
    FieldStoreCensus::setEnabled(true);
    cajeta::Scope::setCapturedBorrowWarns(true);
    try {
        CajetaJit::compile(kSrc, "test.D");
    } catch (...) {
    }
    cajeta::Scope::clearCapturedBorrowWarnsOverride();
    FieldStoreCensus::setEnabled(false);

    std::set<std::string> got;
    std::string interiorType;
    for (auto& r : FieldStoreCensus::records()) {
        if (r.className != "test.Keep") continue;
        got.insert(key(r));
        if (r.methodName == "interior") interiorType = r.type;
    }
    FieldStoreCensus::clear();

    std::set<std::string> want = {
        "plainFormal field = formal-plain p",
        "sharpFormal field #= formal-sharp p",
        "legacy field =# formal-sharp p",
        "heapLocal field #= local-heap l",
        "aliasLocal field = local-alias-of-formal-plain l",
        "interior field = interior-of-formal-plain h",
        "slot slot #= formal-plain p",
        "toStatic static #= formal-plain p",
        "nested field-of-formal #= formal-plain p",
    };
    EXPECT_EQ(got, want);
    EXPECT_EQ(interiorType, "test.Cell");
}
