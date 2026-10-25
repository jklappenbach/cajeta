// C++ JSON competitors for bench/codec/json/Json.md: simdjson and yyjson on
// the nativejson corpus (twitter, citm_catalog, canada) and the JSON_checker
// set, one v1 row per (id, library, file). See the unit contract: tokenize,
// bind-skip, dom, serialize, roundtrip, conformance, each with the page's check.
//
// simdjson: DOM (dom::parser) for dom / serialize (simdjson::minify) /
// roundtrip / conformance; On-Demand for bind-skip (iterate the root object's
// fields, every value skipped) and for tokenize, which has no token stream in
// simdjson: the closest honest analog is an On-Demand walk counting container
// start/end, keys and scalars (scalars touched via raw_json_token).
// yyjson: read/write DOM; it has neither a pull tokenizer nor a typed binder,
// so those two ids are skipped rows with the reason.
#include "simdjson.h"
#include "yyjson.h"
#include "row.h"

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using namespace simdjson;

static volatile size_t g_sink = 0;

static const std::string SIMD_VER = std::string(SIMDJSON_VERSION) + " @9b33047";
static const std::string YY_VER = YYJSON_VERSION_STRING;

struct Corpus { const char* name; long long tokens; long long nodes; };
static const Corpus CORPUS[] = {
    {"twitter", 29573, 13914},
    {"citm_catalog", 85035, 37778},
    {"canada", 223236, 167179},
};

// ---- simdjson helpers ---------------------------------------------------
static long long dom_nodes(dom::element e) {
    switch (e.type()) {
        case dom::element_type::ARRAY: {
            long long n = 1;
            for (dom::element c : dom::array(e)) n += dom_nodes(c);
            return n;
        }
        case dom::element_type::OBJECT: {
            long long n = 1;
            for (dom::key_value_pair kv : dom::object(e)) n += dom_nodes(kv.value);
            return n;
        }
        default: return 1;
    }
}

static bool dom_equal(dom::element a, dom::element b) {
    if (a.type() != b.type()) return false;
    switch (a.type()) {
        case dom::element_type::ARRAY: {
            dom::array x = a, y = b;
            if (x.size() != y.size()) return false;
            auto i = x.begin(), j = y.begin();
            for (; i != x.end(); ++i, ++j) if (!dom_equal(*i, *j)) return false;
            return true;
        }
        case dom::element_type::OBJECT: {
            dom::object x = a, y = b;
            if (x.size() != y.size()) return false;
            auto i = x.begin(), j = y.begin();
            for (; i != x.end(); ++i, ++j) {
                if (i.key() != j.key()) return false;
                if (!dom_equal(i.value(), j.value())) return false;
            }
            return true;
        }
        case dom::element_type::INT64: return int64_t(a) == int64_t(b);
        case dom::element_type::UINT64: return uint64_t(a) == uint64_t(b);
        case dom::element_type::DOUBLE: return double(a) == double(b);
        case dom::element_type::STRING: return std::string_view(a) == std::string_view(b);
        case dom::element_type::BOOL: return bool(a) == bool(b);
        case dom::element_type::NULL_VALUE: return true;
        default: return false;
    }
}

// On-Demand token walk: container start + end, every key, every scalar.
static error_code od_walk(ondemand::value v, long long& n) {
    ondemand::json_type t;
    if (auto e = v.type().get(t)) return e;
    if (t == ondemand::json_type::object) {
        n++;
        ondemand::object o;
        if (auto e = v.get_object().get(o)) return e;
        for (auto f : o) {
            ondemand::field fld;
            if (auto e = std::move(f).get(fld)) return e;
            n++;
            g_sink = g_sink + fld.key().raw()[0];
            if (auto e = od_walk(fld.value(), n)) return e;
        }
        n++;
    } else if (t == ondemand::json_type::array) {
        n++;
        ondemand::array a;
        if (auto e = v.get_array().get(a)) return e;
        for (auto el : a) {
            ondemand::value ev;
            if (auto e = el.get(ev)) return e;
            if (auto e = od_walk(ev, n)) return e;
        }
        n++;
    } else {
        n++;
        g_sink = g_sink + v.raw_json_token().size();
    }
    return SUCCESS;
}

// ---- yyjson helpers -----------------------------------------------------
static long long yy_nodes(yyjson_val* v) {
    long long n = 1;
    if (yyjson_is_arr(v)) {
        size_t i, m;
        yyjson_val* c;
        yyjson_arr_foreach(v, i, m, c) n += yy_nodes(c);
    } else if (yyjson_is_obj(v)) {
        size_t i, m;
        yyjson_val *k, *c;
        yyjson_obj_foreach(v, i, m, k, c) n += yy_nodes(c);
    }
    return n;
}

int main() {
    std::string dir = row::env("PROFILE_DATA_DIR", "");
    if (dir.empty()) {
        std::fprintf(stderr, "cpp/codec/json: PROFILE_DATA_DIR unset\n");
        return 2;
    }
    int warmup = row::env_int("PROFILE_WARMUP", 3);
    int trials = row::env_int("PROFILE_TRIALS", 10);
    if (trials < 1) trials = 1;

    for (const Corpus& c : CORPUS) {
        std::string raw;
        std::string var = c.name;
        if (!row::read_file(dir + "/" + var + ".json", raw)) {
            std::fprintf(stderr, "cpp/codec/json: %s.json missing\n", c.name);
            continue;
        }
        long long nb = (long long)raw.size();
        padded_string ps(raw);

        // ---------------- simdjson ----------------
        {   // json-tokenize (On-Demand walk)
            ondemand::parser op;
            long long tokens = -1;
            bool err = false;
            auto s = row::measure(warmup, trials, [&] {
                ondemand::document doc;
                long long n = 0;
                err = false;
                if (op.iterate(ps).get(doc)) { err = true; return; }
                ondemand::value root;
                if (doc.get_value().get(root) || od_walk(root, n) || !doc.at_end()) err = true;
                tokens = n;
            });
            bool ok = !err && tokens == c.tokens;
            row::emit("json-tokenize", var, nb, nb, "simdjson", SIMD_VER, warmup, trials, s, ok,
                      "no token stream in simdjson: On-Demand walk counting container start/end + keys + scalars; tokens=" + std::to_string(tokens), var);
        }
        {   // json-bind-skip (On-Demand: iterate root fields; values skipped)
            ondemand::parser op;
            bool bound = false;
            auto s = row::measure(warmup, trials, [&] {
                ondemand::document doc;
                bound = false;
                if (op.iterate(ps).get(doc)) return;
                ondemand::object o;
                if (doc.get_object().get(o)) return;
                size_t keys = 0;
                for (auto f : o) {
                    ondemand::field fld;
                    if (std::move(f).get(fld)) return;
                    std::string_view k;
                    if (fld.unescaped_key().get(k)) return;
                    keys += k.size();
                }
                g_sink = g_sink + keys;
                bound = doc.at_end();
            });
            row::emit("json-bind-skip", var, nb, nb, "simdjson", SIMD_VER, warmup, trials, s, bound,
                      "On-Demand: root fields iterated; every value skipped unparsed", var);
        }
        {   // json-dom
            dom::parser p;
            dom::element root;
            bool perr = false;
            auto s = row::measure(warmup, trials, [&] {
                perr = p.parse(ps).get(root) != SUCCESS;
                g_sink = g_sink + (size_t)perr;
            });
            long long nodes = perr ? -1 : dom_nodes(root);
            row::emit("json-dom", var, nb, nb, "simdjson", SIMD_VER, warmup, trials, s, nodes == c.nodes,
                      "dom::parser (reused); nodes=" + std::to_string(nodes), var);
        }
        {   // json-serialize (tree built untimed; simdjson::minify)
            dom::parser p, q;
            dom::element root;
            bool perr = p.parse(ps).get(root) != SUCCESS;
            std::string out;
            auto s = row::measure(warmup, trials, [&] {
                if (!perr) out = simdjson::minify(root);
                g_sink = g_sink + out.size();
            });
            dom::element back;
            long long nodes = (!perr && q.parse(out).get(back) == SUCCESS) ? dom_nodes(back) : -1;
            row::emit("json-serialize", var, nb, nb, "simdjson", SIMD_VER, warmup, trials, s, nodes == c.nodes,
                      "simdjson::minify; out=" + std::to_string(out.size()) + " bytes; reparsed nodes=" + std::to_string(nodes), var);
        }
        {   // json-roundtrip (parse -> minify -> parse)
            dom::parser p, q;
            dom::element a, b;
            std::string out;
            bool err = false;
            auto s = row::measure(warmup, trials, [&] {
                err = p.parse(ps).get(a) != SUCCESS;
                if (err) return;
                out = simdjson::minify(a);
                err = q.parse(out).get(b) != SUCCESS;
                g_sink = g_sink + out.size();
            });
            bool eq = !err && dom_equal(a, b);
            row::emit("json-roundtrip", var, nb, nb, "simdjson", SIMD_VER, warmup, trials, s, eq,
                      "deep equality by a tree walk (simdjson has no element ==)", var);
        }

        // ---------------- yyjson ----------------
        row::skip("json-tokenize", var, nb, nb, "yyjson", YY_VER, "yyjson has no pull tokenizer (DOM reader only)", var);
        row::skip("json-bind-skip", var, nb, nb, "yyjson", YY_VER, "yyjson has no typed binding (DOM reader only)", var);
        {   // json-dom
            yyjson_doc* doc = nullptr;
            auto s = row::measure(warmup, trials, [&] {
                if (doc) yyjson_doc_free(doc);
                doc = yyjson_read(raw.data(), raw.size(), YYJSON_READ_NOFLAG);
                g_sink = g_sink + (size_t)(doc != nullptr);
            });
            long long nodes = doc ? yy_nodes(yyjson_doc_get_root(doc)) : -1;
            row::emit("json-dom", var, nb, nb, "yyjson", YY_VER, warmup, trials, s, nodes == c.nodes,
                      "yyjson_read (previous doc freed in the timed region); nodes=" + std::to_string(nodes), var);
            if (doc) yyjson_doc_free(doc);
        }
        {   // json-serialize
            yyjson_doc* doc = yyjson_read(raw.data(), raw.size(), YYJSON_READ_NOFLAG);
            char* out = nullptr;
            size_t len = 0;
            auto s = row::measure(warmup, trials, [&] {
                if (out) std::free(out);
                out = doc ? yyjson_write(doc, YYJSON_WRITE_NOFLAG, &len) : nullptr;
                g_sink = g_sink + len;
            });
            long long nodes = -1;
            if (out) {
                yyjson_doc* back = yyjson_read(out, len, YYJSON_READ_NOFLAG);
                if (back) { nodes = yy_nodes(yyjson_doc_get_root(back)); yyjson_doc_free(back); }
            }
            row::emit("json-serialize", var, nb, nb, "yyjson", YY_VER, warmup, trials, s, nodes == c.nodes,
                      "yyjson_write; out=" + std::to_string(len) + " bytes; reparsed nodes=" + std::to_string(nodes), var);
            std::free(out);
            if (doc) yyjson_doc_free(doc);
        }
        {   // json-roundtrip
            yyjson_doc *a = nullptr, *b = nullptr;
            auto s = row::measure(warmup, trials, [&] {
                if (a) yyjson_doc_free(a);
                if (b) yyjson_doc_free(b);
                a = yyjson_read(raw.data(), raw.size(), YYJSON_READ_NOFLAG);
                b = nullptr;
                if (!a) return;
                size_t len = 0;
                char* out = yyjson_write(a, YYJSON_WRITE_NOFLAG, &len);
                if (out) { b = yyjson_read(out, len, YYJSON_READ_NOFLAG); std::free(out); }
                g_sink = g_sink + len;
            });
            bool eq = a && b && yyjson_equals(yyjson_doc_get_root(a), yyjson_doc_get_root(b));
            row::emit("json-roundtrip", var, nb, nb, "yyjson", YY_VER, warmup, trials, s, eq,
                      "yyjson_equals on the two trees", var);
            if (a) yyjson_doc_free(a);
            if (b) yyjson_doc_free(b);
        }
    }

    // ---------------- json-conformance (JSON_checker) ----------------
    std::vector<std::string> names;
    for (int i = 1; i <= 3; i++) names.push_back("pass0" + std::to_string(i));
    for (int i = 2; i <= 33; i++) {
        if (i == 18) continue;
        char b[16];
        std::snprintf(b, sizeof b, "fail%02d", i);
        names.push_back(b);
    }
    std::vector<std::string> docs;
    std::vector<padded_string> pdocs;
    bool all = true;
    for (auto& n : names) {
        std::string d;
        if (!row::read_file(dir + "/jsonchecker_" + n + ".json", d)) { all = false; break; }
        pdocs.emplace_back(d);
        docs.push_back(std::move(d));
    }
    if (!all) {
        row::skip("json-conformance", "jsonchecker", -1, 34, "simdjson", SIMD_VER, "jsonchecker files missing (run datasets/fetch.sh)", "");
        row::skip("json-conformance", "jsonchecker", -1, 34, "yyjson", YY_VER, "jsonchecker files missing (run datasets/fetch.sh)", "");
        return 0;
    }
    long long total = 0;
    for (auto& d : docs) total += (long long)d.size();
    auto report = [&](const char* lib, const std::string& ver, const row::Stats& s, const std::vector<bool>& acc, const char* how) {
        int pa = 0, fr = 0;
        std::string mis;
        for (size_t i = 0; i < names.size(); i++) {
            bool pass = names[i].rfind("pass", 0) == 0;
            if (pass && acc[i]) pa++;
            if (!pass && !acc[i]) fr++;
            if (acc[i] != pass) mis += " " + names[i];
        }
        std::string notes = std::string(how) + "; pass " + std::to_string(pa) + "/3 fail " + std::to_string(fr) + "/31";
        if (!mis.empty()) notes += "; misclassified" + mis;
        row::emit("json-conformance", "jsonchecker", total, 34, lib, ver, warmup, trials, s, pa == 3 && fr == 31, notes, "", false);
    };
    {
        dom::parser p;
        std::vector<bool> acc(names.size());
        auto s = row::measure(warmup, trials, [&] {
            for (size_t i = 0; i < pdocs.size(); i++) {
                dom::element e;
                acc[i] = p.parse(pdocs[i]).get(e) == SUCCESS;
            }
        });
        report("simdjson", SIMD_VER, s, acc, "dom::parser");
    }
    {
        std::vector<bool> acc(names.size());
        auto s = row::measure(warmup, trials, [&] {
            for (size_t i = 0; i < docs.size(); i++) {
                yyjson_doc* d = yyjson_read(docs[i].data(), docs[i].size(), YYJSON_READ_NOFLAG);
                acc[i] = d != nullptr;
                if (d) yyjson_doc_free(d);
            }
        });
        report("yyjson", YY_VER, s, acc, "yyjson_read strict (no flags)");
    }
    return 0;
}
