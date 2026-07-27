//
// debug-type-sidecar Unit 1: the DebugTypeTable record model + the builder that
// walks the live type world into records.
//
// The table is what makes a WARM (cache-hit) debug launch inspectable: a hit has
// no type world, so every layout fact ValueInspector needs must be resolved cold
// and carried. These tests build the table from a real compiled program at a
// stop and assert each record reproduces EXACTLY what the live decode uses —
// field byte offsets, element strides, inline-vs-pointer storage — because a
// record that disagrees with the live layout would misread memory warm.
//
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "cajeta/dbg/DebugTypeTable.h"
#include "cajeta/dbg/ValueInspector.h"
#include "cajeta/dbg/DebugVars.h"
#include "cajeta/dbg/DebugController.h"
#include "cajeta/jit/CajetaJitHost.h"
#include "cajeta/type/CajetaType.h"
#include "../TempProgram.h"

using cajeta::jit::Breakpoint;
using cajeta::jit::JitRunOptions;
using cajeta::jit::startDebugSession;
using cajeta::dbg::StopEvent;
using cajeta::dbg::ValueInspector;
using cajeta::dbg::Storage;
using cajeta::dbg::DebugTypeTable;
using cajeta::dbg::TypeKind;
using cajeta::dbg::CollectionKind;
using cajeta::dbg::TypeRecord;
using cajeta::debugtest::TempProgram;

// ---- pure model: lookup tolerance (no JIT) ----

TEST(DebugTypeTableModel, MissingRecordIsNullNotAFault) {
    DebugTypeTable t;
    EXPECT_TRUE(t.empty());
    EXPECT_EQ(t.find("nope.NotAType"), nullptr);
    EXPECT_EQ(t.find(""), nullptr);
    EXPECT_EQ(t.find("int32[]"), nullptr);

    TypeRecord r;
    r.canonical = "int32";
    r.kind = TypeKind::Leaf;
    t.put(r);
    ASSERT_NE(t.find("int32"), nullptr);
    EXPECT_EQ(t.find("int32")->kind, TypeKind::Leaf);
    EXPECT_EQ(t.find("int64"), nullptr);   // still a clean miss
    t.clear();
    EXPECT_EQ(t.find("int32"), nullptr);
}

// ---- Unit 3: sidecar serialize / deserialize (pure, no JIT) ----

namespace {
// A small hand-built table exercising every record shape and the alias filing
// (a record stored under a non-canonical key), plus the String ABI.
cajeta::dbg::DebugTypeTable makeSampleTable() {
    using namespace cajeta::dbg;
    DebugTypeTable t;

    StringAbi abi;
    abi.valid = true;
    abi.size = 64;
    abi.offLenTag = 8;
    abi.offAux = 12;
    abi.offBase = 16;
    t.setStringAbi(abi);

    TypeRecord i32;
    i32.canonical = "int32";
    i32.kind = TypeKind::Leaf;
    i32.isValueType = true;
    t.put(i32);

    TypeRecord str;
    str.canonical = "cajeta.lang.String";
    str.kind = TypeKind::Leaf;
    str.isString = true;
    t.put(str);

    TypeRecord pt;
    pt.canonical = "demo.Point";
    pt.kind = TypeKind::Object;
    pt.fields.push_back({"x", "int32", 8, Storage::Inline});
    pt.fields.push_back({"y", "int32", 12, Storage::Inline});
    t.put(pt);
    // Alias filing: the short name maps to the same record body.
    t.putAs("Point", pt);

    TypeRecord arr;
    arr.canonical = "demo.Point[]";
    arr.kind = TypeKind::Array;
    arr.elem = {"demo.Point", 16, Storage::Pointer};
    t.put(arr);

    TypeRecord list;
    list.canonical = "cajeta.collection.ArrayList<int32>";
    list.kind = TypeKind::Collection;
    list.collectionKind = CollectionKind::ArrayList;
    list.fields.push_back({"data", "int32[]", 8, Storage::Pointer});
    list.fields.push_back({"sizeCount", "int32", 16, Storage::Inline});
    t.put(list);

    return t;
}

std::string sidecarPath(const char* tag) {
    return (std::filesystem::temp_directory_path() /
            (std::string("cajeta_typeinfo_test_") + tag + ".tsv")).string();
}

void expectTablesEqual(const cajeta::dbg::DebugTypeTable& a,
                       const cajeta::dbg::DebugTypeTable& b) {
    using namespace cajeta::dbg;
    ASSERT_EQ(a.size(), b.size());
    EXPECT_EQ(a.stringAbi().valid, b.stringAbi().valid);
    EXPECT_EQ(a.stringAbi().size, b.stringAbi().size);
    EXPECT_EQ(a.stringAbi().offLenTag, b.stringAbi().offLenTag);
    EXPECT_EQ(a.stringAbi().offAux, b.stringAbi().offAux);
    EXPECT_EQ(a.stringAbi().offBase, b.stringAbi().offBase);
    for (const auto& name : a.names()) {
        const TypeRecord* ra = a.find(name);
        const TypeRecord* rb = b.find(name);
        ASSERT_NE(rb, nullptr) << "missing after round-trip: " << name;
        EXPECT_EQ(ra->canonical, rb->canonical) << name;
        EXPECT_EQ(ra->kind, rb->kind) << name;
        EXPECT_EQ(ra->isValueType, rb->isValueType) << name;
        EXPECT_EQ(ra->isString, rb->isString) << name;
        EXPECT_EQ(ra->collectionKind, rb->collectionKind) << name;
        EXPECT_EQ(ra->elem.type, rb->elem.type) << name;
        EXPECT_EQ(ra->elem.stride, rb->elem.stride) << name;
        EXPECT_EQ(ra->elem.storage, rb->elem.storage) << name;
        ASSERT_EQ(ra->fields.size(), rb->fields.size()) << name;
        for (size_t i = 0; i < ra->fields.size(); i++) {
            EXPECT_EQ(ra->fields[i].name, rb->fields[i].name) << name;
            EXPECT_EQ(ra->fields[i].type, rb->fields[i].type) << name;
            EXPECT_EQ(ra->fields[i].offset, rb->fields[i].offset) << name;
            EXPECT_EQ(ra->fields[i].storage, rb->fields[i].storage) << name;
        }
    }
}
} // namespace

// 3.1.1 — write to bytes, read into a fresh table, record-for-record equal
// (alias keys and the String ABI included).
TEST(DebugTypeTableSidecar, RoundTrips) {
    auto t = makeSampleTable();
    const std::string path = sidecarPath("roundtrip");
    ASSERT_TRUE(cajeta::dbg::writeTypeSidecar(path, t));

    cajeta::dbg::DebugTypeTable back;
    ASSERT_TRUE(cajeta::dbg::loadTypeSidecar(path, back));
    expectTablesEqual(t, back);
    std::filesystem::remove(path);
}

// 3.1.2 — an unknown schema major loads as an EMPTY table, never a misread.
TEST(DebugTypeTableSidecar, RefusesUnknownMajor) {
    auto t = makeSampleTable();
    const std::string path = sidecarPath("major");
    ASSERT_TRUE(cajeta::dbg::writeTypeSidecar(path, t));
    // Bump the version line to a major this reader does not know.
    {
        std::ifstream in(path);
        std::string all((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
        in.close();
        auto nl = all.find('\n');
        ASSERT_NE(nl, std::string::npos);
        std::ofstream out(path, std::ios::trunc);
        out << "cajeta-typeinfo-v999" << all.substr(nl);
    }
    cajeta::dbg::DebugTypeTable back;
    // Poison it first to prove a refused load leaves it EMPTY, not partial.
    back.put([]{ cajeta::dbg::TypeRecord r; r.canonical = "stale.Type"; return r; }());
    EXPECT_FALSE(cajeta::dbg::loadTypeSidecar(path, back));
    EXPECT_TRUE(back.empty());
    std::filesystem::remove(path);
}

// 3.1.3 — truncation/corruption yields an empty table, not a crash or misread.
TEST(DebugTypeTableSidecar, ToleratesTruncation) {
    auto t = makeSampleTable();
    const std::string path = sidecarPath("trunc");
    ASSERT_TRUE(cajeta::dbg::writeTypeSidecar(path, t));

    // Truncate mid-record.
    std::error_code ec;
    auto full = std::filesystem::file_size(path, ec);
    ASSERT_FALSE(ec);
    std::filesystem::resize_file(path, full / 2, ec);
    ASSERT_FALSE(ec);

    cajeta::dbg::DebugTypeTable back;
    back.put([]{ cajeta::dbg::TypeRecord r; r.canonical = "stale.Type"; return r; }());
    EXPECT_FALSE(cajeta::dbg::loadTypeSidecar(path, back));
    EXPECT_TRUE(back.empty());

    // Garbage bytes: same clean refusal.
    { std::ofstream out(path, std::ios::trunc); out << "\x7f\x03garbage\tnot\ta\ttable\n"; }
    EXPECT_FALSE(cajeta::dbg::loadTypeSidecar(path, back));
    EXPECT_TRUE(back.empty());

    // A missing file is false + empty, never a throw.
    std::filesystem::remove(path);
    EXPECT_FALSE(cajeta::dbg::loadTypeSidecar(path, back));
    EXPECT_TRUE(back.empty());
}

// ---- built from a real compiled program at a stop ----

namespace {
// Lines:                1                 2                  3
const char* kProg =
    "package demo;\n"                                              // 1
    "public class Point { public int32 x; public int32 y;\n"        // 2
    "    public Point(int32 a, int32 b) { this.x = a; this.y = b; } }\n" // 3
    "public record Vec2 { int32 a; int32 b; }\n"                    // 4 value type
    "public class A { public int32 a; public A() { return; } }\n"   // 5
    "public class B { public int32 b; public B() { return; } }\n"   // 6
    "public class C extends A, B {\n"                               // 7
    "    public C() { this.a = 7; this.b = 11; } }\n"               // 8 2nd vtable
    "public class Holder { public Point p; public int32 n;\n"       // 9
    "    public Holder() { this.p = heap Point(1, 2); this.n = 99; } }\n" // 10
    "public class Unused { public int32 z; public Unused() { this.z = 1; } }\n" // 11
    "public class Probe {\n"                                        // 12
    "    public static int32 main() {\n"                            // 13
    "        Point pt = heap Point(3, 4);\n"                        // 14
    "        Vec2 v = {a:8, b:9};\n"                                // 15
    "        C c = heap C();\n"                                     // 16
    "        Holder h = heap Holder();\n"                           // 17
    "        int32[] nums = [3, 7, 9];\n"                           // 18
    "        String[] strs = [\"a\", \"bb\"];\n"                    // 19
    "        Point[] pts = [heap Point(1, 2), heap Point(3, 4)];\n" // 20
    "        ArrayList<int32> xs = [10, 20, 30];\n"                 // 21
    "        HashMap<int32,int32> m = [1:100, 2:200];\n"            // 22
    "        String s = \"hi\";\n"                                  // 23
    "        int32 done = 0;\n"                                     // 24
    "        return done;\n"                                        // 25 <-- bp
    "    }\n"                                                       // 26
    "}\n";                                                          // 27

const cajeta::dbg::FieldRecord* fieldNamed(const TypeRecord& r,
                                           const std::string& name) {
    for (const auto& f : r.fields) if (f.name == name) return &f;
    return nullptr;
}

const cajeta::dbg::InspectedChild* childNamed(
        const std::vector<cajeta::dbg::InspectedChild>& cs,
        const std::string& name) {
    for (const auto& c : cs) if (c.name == name) return &c;
    return nullptr;
}
} // namespace

class DebugTypeTableStop : public ::testing::Test {
protected:
    TempProgram prog{"demo", "Probe.cajeta", kProg};
    std::unique_ptr<cajeta::jit::JitDebugSession> session;
    const llvm::DataLayout* dl = nullptr;
    std::vector<cajeta::dbg::DbgFrameInfo> frames;
    DebugTypeTable table;

    const cajeta::dbg::DbgVar* local(const std::string& name) const {
        for (const auto& v : frames[0].locals)
            if (v.name == name) return &v;
        return nullptr;
    }

    void SetUp() override {
        JitRunOptions opts;
        opts.sourceRoot = prog.sourceRoot();
        opts.entryMethod = "demo.Probe.main";
        std::vector<Breakpoint> bps{ Breakpoint{"Probe.cajeta", 25} };
        std::string err;
        session = startDebugSession(opts, bps, &err);
        ASSERT_NE(session, nullptr) << err;
        StopEvent ev;
        ASSERT_TRUE(session->controller().waitForStop(
            ev, std::chrono::seconds(30))) << "never hit the breakpoint";
        ASSERT_NE(ev.frameTop, nullptr);
        dl = &session->dataLayout();
        frames = cajeta::dbg::walkFrames(ev.frameTop);
        ASSERT_FALSE(frames.empty());

        // Roots are exactly what a debug local can be — the same set codegen
        // registers (Unit 4). The closure walk expands them.
        for (const auto& v : frames[0].locals) table.addRoot(v.type);
        table.buildFromTypeWorld(*dl);
        ASSERT_FALSE(table.empty());
    }

    void TearDown() override {
        if (session) {
            session->controller().resume();
            session->join();
        }
    }
};

// 1.1.1 — a primitive and String are Leaf records; String keeps the String-ABI
// facts (it is decoded by its ABI, not by field walking).
TEST_F(DebugTypeTableStop, BuildsLeafAndStringRecords) {
    const auto* i32 = table.find("int32");
    ASSERT_NE(i32, nullptr);
    EXPECT_EQ(i32->kind, TypeKind::Leaf);
    EXPECT_TRUE(i32->fields.empty());

    const auto* str = table.find("cajeta.lang.String");
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(str->kind, TypeKind::Leaf);
    EXPECT_TRUE(str->fields.empty());
    EXPECT_FALSE(str->isValueType);   // a reference class: a pointer slot
    // The String-ness is a carried fact, so a decoder never has to match the
    // stdlib name; its decode ABI rides the table too.
    EXPECT_TRUE(str->isString);
    EXPECT_FALSE(i32->isString);
    EXPECT_TRUE(table.stringAbi().valid);
    EXPECT_GT(table.stringAbi().offBase, table.stringAbi().offLenTag);
}

// 1.1.2 — a class yields an Object record with ordered fields carrying declared
// name, type, byte offset and storage; a record (@ValueType) is isValueType with
// its first field at offset 0 (no slot-0 vtable word).
TEST_F(DebugTypeTableStop, BuildsObjectFieldRecords) {
    const auto* pt = table.find("demo.Point");
    ASSERT_NE(pt, nullptr);
    EXPECT_EQ(pt->kind, TypeKind::Object);
    EXPECT_FALSE(pt->isValueType);
    ASSERT_EQ(pt->fields.size(), 2u);
    EXPECT_EQ(pt->fields[0].name, "x");
    EXPECT_EQ(pt->fields[1].name, "y");
    EXPECT_EQ(pt->fields[0].type, "int32");
    EXPECT_EQ(pt->fields[0].storage, Storage::Inline);
    EXPECT_GT(pt->fields[0].offset, 0u);      // past the vtable pointer
    EXPECT_GT(pt->fields[1].offset, pt->fields[0].offset);

    // A plain class is reachable by its short name too — everything
    // CajetaType::of resolved, the table resolves.
    const auto* ptShort = table.find("Point");
    ASSERT_NE(ptShort, nullptr);
    EXPECT_EQ(ptShort->canonical, "demo.Point");
    EXPECT_EQ(ptShort->fields.size(), pt->fields.size());
    EXPECT_EQ(ptShort->fields[0].offset, pt->fields[0].offset);

    const auto* v2 = table.find("demo.Vec2");
    ASSERT_NE(v2, nullptr);
    EXPECT_EQ(v2->kind, TypeKind::Object);
    EXPECT_TRUE(v2->isValueType);
    ASSERT_EQ(v2->fields.size(), 2u);
    EXPECT_EQ(v2->fields[0].name, "a");
    EXPECT_EQ(v2->fields[0].offset, 0u);      // a POD starts at its own base

    // A reference-class field is a pointer slot; a primitive field is inline.
    const auto* holder = table.find("demo.Holder");
    ASSERT_NE(holder, nullptr);
    const auto* fp = fieldNamed(*holder, "p");
    const auto* fn = fieldNamed(*holder, "n");
    ASSERT_NE(fp, nullptr);
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fp->type, "demo.Point");
    EXPECT_EQ(fp->storage, Storage::Pointer);
    EXPECT_EQ(fn->storage, Storage::Inline);
}

// 1.1.3 — array records carry element type, per-element slot stride and storage,
// matching how the JIT'd code stores elements (String keeps its 64-byte slot).
TEST_F(DebugTypeTableStop, BuildsArrayRecord) {
    const auto* ints = table.find("int32[]");
    ASSERT_NE(ints, nullptr);
    EXPECT_EQ(ints->kind, TypeKind::Array);
    EXPECT_EQ(ints->elem.type, "int32");
    EXPECT_EQ(ints->elem.stride, 4u);
    EXPECT_EQ(ints->elem.storage, Storage::Inline);

    const auto* strs = table.find("cajeta.lang.String[]");
    ASSERT_NE(strs, nullptr);
    EXPECT_EQ(strs->kind, TypeKind::Array);
    EXPECT_EQ(strs->elem.type, "cajeta.lang.String");
    EXPECT_EQ(strs->elem.storage, Storage::Pointer);   // String* at the base
    EXPECT_GT(strs->elem.stride, 8u);                  // the full struct slot

    const auto* pts = table.find("demo.Point[]");
    ASSERT_NE(pts, nullptr);
    EXPECT_EQ(pts->kind, TypeKind::Array);
    EXPECT_EQ(pts->elem.type, "demo.Point");
    // A reference-class element keeps its OWN struct slot with the instance
    // pointer at the base (like String), so the stride is the struct's alloc
    // size, not a bare pointer. The exact value is pinned against the live
    // layout in OffsetsMatchLiveLayout.
    EXPECT_EQ(pts->elem.storage, Storage::Pointer);
    EXPECT_GE(pts->elem.stride, dl->getPointerSize());
}

// 1.1.4 — a registered collection is tagged with its kind AND keeps its object
// fields, so the logical view still finds data/sizeCount/slots/ctrl by name.
TEST_F(DebugTypeTableStop, BuildsCollectionRecord) {
    const auto* xs = local("xs");
    const auto* m = local("m");
    ASSERT_NE(xs, nullptr);
    ASSERT_NE(m, nullptr);

    const auto* list = table.find(xs->type);
    ASSERT_NE(list, nullptr) << "no record for " << xs->type;
    EXPECT_EQ(list->kind, TypeKind::Collection);
    EXPECT_EQ(list->collectionKind, CollectionKind::ArrayList);
    ASSERT_NE(fieldNamed(*list, "data"), nullptr);
    ASSERT_NE(fieldNamed(*list, "sizeCount"), nullptr);
    // the backing array's own record is reachable through the `data` field
    EXPECT_NE(table.find(fieldNamed(*list, "data")->type), nullptr);

    const auto* map = table.find(m->type);
    ASSERT_NE(map, nullptr) << "no record for " << m->type;
    EXPECT_EQ(map->kind, TypeKind::Collection);
    EXPECT_EQ(map->collectionKind, CollectionKind::HashMap);
    ASSERT_NE(fieldNamed(*map, "slots"), nullptr);
    ASSERT_NE(fieldNamed(*map, "ctrl"), nullptr);
}

// 1.1.5 — THE test that makes the sidecar safe: every stored offset/stride is
// the one the live decode uses. Compared against ValueInspector's own child
// addresses at this stop (the multi-parent interior-vtable field included).
TEST_F(DebugTypeTableStop, OffsetsMatchLiveLayout) {
    ValueInspector insp(*dl);

    struct { const char* localName; const char* type; } objs[] = {
        {"pt", "demo.Point"}, {"c", "demo.C"}, {"h", "demo.Holder"},
    };
    for (const auto& o : objs) {
        const auto* v = local(o.localName);
        ASSERT_NE(v, nullptr) << o.localName;
        const auto* rec = table.find(o.type);
        ASSERT_NE(rec, nullptr) << o.type;
        auto page = insp.children(v->type, v->addr);
        ASSERT_FALSE(page.children.empty()) << o.type;

        // A reference class slot holds the instance pointer; a value type is
        // the instance. Table offset + base must equal the live child address.
        char* inst = rec->isValueType
            ? reinterpret_cast<char*>(v->addr)
            : *reinterpret_cast<char**>(v->addr);
        ASSERT_NE(inst, nullptr) << o.type;
        for (const auto& f : rec->fields) {
            const auto* live = childNamed(page.children, f.name);
            ASSERT_NE(live, nullptr) << o.type << "." << f.name;
            EXPECT_EQ(inst + f.offset, reinterpret_cast<char*>(live->addr))
                << o.type << "." << f.name << " offset " << f.offset;
            EXPECT_EQ(f.storage, live->storage) << o.type << "." << f.name;
            EXPECT_EQ(f.type, live->type) << o.type << "." << f.name;
        }
    }

    // Element strides: the table's stride is the live slot-to-slot distance.
    struct { const char* localName; const char* type; } arrs[] = {
        {"nums", "int32[]"}, {"strs", "cajeta.lang.String[]"},
        {"pts", "demo.Point[]"},
    };
    for (const auto& a : arrs) {
        const auto* v = local(a.localName);
        ASSERT_NE(v, nullptr) << a.localName;
        const auto* rec = table.find(a.type);
        ASSERT_NE(rec, nullptr) << a.type;
        auto page = insp.children(v->type, v->addr);
        ASSERT_GE(page.children.size(), 2u) << a.type;
        auto d0 = reinterpret_cast<char*>(page.children[0].addr);
        auto d1 = reinterpret_cast<char*>(page.children[1].addr);
        EXPECT_EQ(static_cast<uint64_t>(d1 - d0), rec->elem.stride) << a.type;
        EXPECT_EQ(rec->elem.storage, page.children[0].storage) << a.type;
        EXPECT_EQ(rec->elem.type, page.children[0].type) << a.type;
    }
}

// 3.1.1 (authentic form) — the table built from the live program round-trips
// through the sidecar byte-for-byte in every record, so a warm load decodes
// from exactly the facts the cold build resolved.
TEST_F(DebugTypeTableStop, BuiltTableRoundTrips) {
    const std::string path = sidecarPath("built");
    ASSERT_TRUE(cajeta::dbg::writeTypeSidecar(path, table));
    cajeta::dbg::DebugTypeTable back;
    ASSERT_TRUE(cajeta::dbg::loadTypeSidecar(path, back));
    expectTablesEqual(table, back);
    std::filesystem::remove(path);
}

// 1.1.6 — the closure covers a local's type, its fields' types transitively and
// array/collection element types; a type never referenced is absent. Bounding
// the walk is reported, never silent.
TEST_F(DebugTypeTableStop, ReachableClosure) {
    EXPECT_NE(table.find("demo.Holder"), nullptr);     // a local's type
    EXPECT_NE(table.find("demo.Point"), nullptr);      // Holder.p, transitively
    EXPECT_NE(table.find("demo.Point[]"), nullptr);    // a local's array type
    EXPECT_NE(table.find("int32"), nullptr);           // a field's type
    EXPECT_NE(table.find("cajeta.lang.String"), nullptr);  // an element type
    EXPECT_TRUE(table.bounded().empty());              // nothing was dropped

    // `Unused` is a live type in this program but no debug value can be one,
    // so it is not carried.
    EXPECT_NE(cajeta::CajetaType::of("demo.Unused"), nullptr)
        << "fixture drift: Unused should exist in the type world";
    EXPECT_EQ(table.find("demo.Unused"), nullptr);

    // A bounded walk reports what it dropped rather than silently truncating.
    DebugTypeTable small;
    for (const auto& v : frames[0].locals) small.addRoot(v.type);
    cajeta::dbg::BuildOptions opts;
    opts.maxRecords = 2;
    small.buildFromTypeWorld(*dl, opts);
    EXPECT_LE(small.size(), 2u);
    EXPECT_FALSE(small.bounded().empty());
}
