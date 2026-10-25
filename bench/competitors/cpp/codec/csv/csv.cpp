// C++ CSV competitor for bench/codec/csv/Csv.md: fast-cpp-csv-parser
// (ben-strasser, header-only) reading the generated file
// (datasets/gen_csv.py) from memory into typed rows
// (int64 id, string name, int32 qty, double price, string note), RFC 4180
// double-quote escaping, no trimming, single-threaded (CSV_IO_NO_THREAD: the
// library's read-ahead thread only helps when the bytes come from disk, and
// every other language parses from memory). Check: row count and the sum of
// qty against the .meta file. The library has no writer, so csv-write is a
// skipped row with that reason.
#define CSV_IO_NO_THREAD
#include "csv.h"
#include "row.h"

#include <cstdint>
#include <string>
#include <vector>

struct Row {
    int64_t id;
    std::string name;
    int32_t qty;
    double price;
    std::string note;
};

static volatile size_t g_sink = 0;

int main() {
    std::string dir = row::env("PROFILE_DATA_DIR", "");
    std::string smoke = row::env("CAJETA_BENCH_SMOKE", "");
    bool isSmoke = smoke == "1" || smoke == "true" || smoke == "yes";
    std::string scale = isSmoke ? "smoke" : "full";
    std::string ds = "codec-csv-" + scale;
    const char* lib = "fast-cpp-csv-parser";
    const std::string ver = "master @574a9fe";
    std::string extra = isSmoke ? "; smoke input 4 MB" : "";

    std::string raw, meta;
    if (dir.empty() || !row::read_file(dir + "/" + ds + ".csv", raw) || !row::read_file(dir + "/" + ds + ".meta", meta)) {
        row::skip("csv-parse", ds, -1, -1, lib, ver, "input missing (run datasets/gen_csv.py)", "");
        row::skip("csv-write", ds, -1, -1, lib, ver, "input missing (run datasets/gen_csv.py)", "");
        return 0;
    }
    long long wantRows = 0, wantQty = 0, wantBytes = 0;
    std::sscanf(meta.c_str(), "%lld %lld %lld", &wantRows, &wantQty, &wantBytes);
    long long nb = (long long)raw.size();

    int warmup = row::env_int("PROFILE_WARMUP", 3);
    int trials = row::env_int("PROFILE_TRIALS", 10);
    if (trials < 1) trials = 1;

    std::vector<Row> rows;
    bool err = false;
    std::string why;
    auto s = row::measure(warmup, trials, [&] {
        std::vector<Row> out;
        err = false;
        try {
            io::CSVReader<5, io::trim_chars<>, io::double_quote_escape<',', '"'>> in(
                ds + ".csv", raw.data(), raw.data() + raw.size());
            in.read_header(io::ignore_no_column, "id", "name", "qty", "price", "note");
            Row r;
            while (in.read_row(r.id, r.name, r.qty, r.price, r.note)) out.push_back(r);
        } catch (const std::exception& e) {
            err = true;
            why = e.what();
        }
        g_sink = g_sink + out.size();
        rows = std::move(out);
    });
    long long qty = 0;
    for (const Row& r : rows) qty += r.qty;
    bool ok = !err && (long long)rows.size() == wantRows && qty == wantQty;
    std::string notes = "CSVReader<5> double_quote_escape; rows=" + std::to_string(rows.size()) +
                        " fields=" + std::to_string(rows.size() * 5) + " qty_sum=" + std::to_string(qty) + extra;
    if (err) notes += "; error: " + why;
    for (char& c : notes) if (c == ',') c = ';';
    row::emit("csv-parse", ds, nb, nb, lib, ver, warmup, trials, s, ok, notes, "");
    row::skip("csv-write", ds, nb, nb, lib, ver, "fast-cpp-csv-parser is a reader only (no writer)", "");
    return 0;
}
