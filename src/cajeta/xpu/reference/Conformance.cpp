// The kernel conformance corpus runner: see Conformance.h.

#include "Conformance.h"
#include "KernelInterpreter.h"

#include "../core/KernelManifest.h"
#include "../../error/Exception.h"
#include "../../method/Method.h"

#include "llvm/Support/JSON.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <sstream>

namespace fs = std::filesystem;

namespace cajeta {
namespace xpu {
namespace reference {

namespace {

bool readFile(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

struct Held {
    bool held = false;
    std::string note;
    uint64_t ulps = 0;
};

// kernel -> backend -> entry; backend "*" matches every backend.
using HeldList = std::map<std::string, std::map<std::string, Held>>;

HeldList readHeld(const std::string& path) {
    HeldList out;
    if (path.empty()) return out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, '\t')) f.push_back(cell);
        if (f.size() < 3) continue;
        Held& h = out[f[0]][f[1]];
        if (f[2] == "held") {
            h.held = true;
        } else if (f[2].rfind("ulps=", 0) == 0) {
            h.ulps = std::stoull(f[2].substr(5));
        }
        if (f.size() > 3) h.note = f[3];
    }
    return out;
}

Held heldFor(const HeldList& list, const std::string& kernel, const std::string& backend) {
    auto k = list.find(kernel);
    if (k == list.end()) return {};
    if (auto b = k->second.find(backend); b != k->second.end()) return b->second;
    if (auto b = k->second.find("*"); b != k->second.end()) return b->second;
    return {};
}

// Units in the last place between two floats of `bytes` width, given as
// their bit patterns. Sign-magnitude, so one formula serves float16,
// bfloat16, float32 and float64.
uint64_t ulps(uint64_t a, uint64_t b, unsigned bytes) {
    const uint64_t sign = 1ull << (bytes * 8 - 1);
    const uint64_t mag = sign - 1;
    auto key = [&](uint64_t x) -> __int128 {
        return (x & sign) ? -(__int128) (x & mag) : (__int128) (x & mag);
    };
    __int128 d = key(a) - key(b);
    return (uint64_t) (d < 0 ? -d : d);
}

uint64_t loadBits(const uint8_t* p, unsigned bytes) {
    uint64_t v = 0;
    std::memcpy(&v, p, bytes);
    return v;
}

// Is the float of `bytes` width with these bits a NaN? float16 and bfloat16
// are told apart by `bf16`.
bool isNanBits(uint64_t x, unsigned bytes, bool bf16) {
    unsigned eb = bytes == 8 ? 11 : bytes == 4 ? 8 : (bf16 ? 8 : 5);
    unsigned man = bytes * 8 - 1 - eb;
    uint64_t e = (x >> man) & ((1ull << eb) - 1);
    return e == ((1ull << eb) - 1) && (x & ((1ull << man) - 1)) != 0;
}

std::string hex(uint64_t v, unsigned bytes) {
    std::ostringstream os;
    os << "0x" << std::hex;
    os.width(bytes * 2);
    os.fill('0');
    os << v;
    return os.str();
}

} // namespace

size_t CorpusRun::failures() const {
    size_t n = 0;
    for (auto& r : results)
        if (r.outcome == "fail" || r.outcome == "stale" || r.outcome == "undefined") ++n;
    return n;
}

CorpusRun runCorpus(const std::vector<MethodPtr>& kernels, const std::string& recordDir,
                    const std::string& heldPath, double budgetSeconds, std::ostream* log) {
    CorpusRun run;
    HeldList held = readHeld(heldPath);
    std::map<std::string, MethodPtr> byName;
    for (auto& k : kernels)
        if (k) byName[kernelRegistryName(k)] = k;

    std::vector<fs::path> launches;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(recordDir, ec))
        if (e.is_directory() && fs::exists(e.path() / "launch.json")) launches.push_back(e.path());
    std::sort(launches.begin(), launches.end());

    for (const fs::path& dir : launches) {
        CorpusResult r;
        r.launch = dir.filename().string();
        std::vector<uint8_t> raw;
        readFile(dir / "launch.json", raw);
        auto parsed = llvm::json::parse(llvm::StringRef((const char*) raw.data(), raw.size()));
        if (!parsed || !parsed->getAsObject()) {
            llvm::consumeError(parsed.takeError());
            r.outcome = "undefined";
            r.detail = "launch.json does not parse";
            run.results.push_back(r);
            continue;
        }
        const llvm::json::Object& L = *parsed->getAsObject();
        r.kernel = L.getString("kernel").value_or("").str();
        r.backend = L.getString("backend").value_or("").str();
        auto k = byName.find(r.kernel);
        if (k == byName.end()) {
            r.outcome = "missing";
            r.detail = "no kernel of that name in this build";
            run.results.push_back(r);
            continue;
        }
        const MethodPtr& kernel = k->second;
        std::vector<ParamShape> shapes = paramShapes(kernel);

        // The allocations as the backend saw them before the launch: the
        // interpreter's memory. One host copy per allocation, so arguments
        // that alias stay aliased.
        std::vector<std::vector<uint8_t>> mem, after;
        const llvm::json::Array* allocs = L.getArray("allocs");
        bool ok = allocs != nullptr;
        for (size_t i = 0; ok && i < allocs->size(); ++i) {
            mem.emplace_back();
            after.emplace_back();
            ok = readFile(dir / ("a" + std::to_string(i) + ".in"), mem.back())
                && readFile(dir / ("a" + std::to_string(i) + ".out"), after.back());
        }
        const llvm::json::Array* args = L.getArray("args");
        if (!ok || !args || args->size() != shapes.size()) {
            r.outcome = "undefined";
            r.detail = !ok ? "an allocation file is missing"
                           : "the recording's arguments do not match the kernel's parameters";
            run.results.push_back(r);
            continue;
        }
        std::vector<Arg> argv;
        // Which recorded allocation, and where in it, each buffer parameter reads.
        std::vector<std::pair<int64_t, int64_t>> where(shapes.size(), {-1, 0});
        for (size_t i = 0; i < args->size(); ++i) {
            const llvm::json::Object* a = (*args)[i].getAsObject();
            std::string kind = a ? a->getString("kind").value_or("").str() : "";
            if (kind == "buffer") {
                int64_t al = a->getInteger("alloc").value_or(-1);
                int64_t off = a->getInteger("offset").value_or(0);
                if (al < 0 || (size_t) al >= mem.size()) { ok = false; break; }
                where[i] = {al, off};
                argv.push_back(Arg::bufferBytes(mem[al].data() + off,
                                                mem[al].size() - (size_t) off));
            } else if (kind == "scalar") {
                std::string h = a->getString("hex").value_or("").str();
                uint64_t bits = 0;
                for (size_t q = 0; q + 1 < h.size() && q / 2 < 8; q += 2)
                    bits |= (uint64_t) std::stoul(h.substr(q, 2), nullptr, 16) << (4 * q);
                argv.push_back(Arg::scalar(bits));
            } else {
                ok = false;
                break;
            }
        }
        if (!ok) {
            r.outcome = "refused";
            r.detail = "an argument the corpus cannot replay (not a buffer or a scalar)";
            run.results.push_back(r);
            continue;
        }
        Launch launch;
        const llvm::json::Array* g = L.getArray("grid");
        const llvm::json::Array* b = L.getArray("block");
        for (size_t d = 0; d < 3; ++d) {
            launch.grid[d] = (uint32_t) (*g)[d].getAsInteger().value_or(1);
            launch.block[d] = (uint32_t) (*b)[d].getAsInteger().value_or(1);
        }
        launch.waveWidth = (uint32_t) L.getInteger("waveWidth").value_or(0);
        launch.budgetSeconds = budgetSeconds;

        auto t0 = std::chrono::steady_clock::now();
        auto progress = [&]() {
            if (!log) return;
            double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            (*log) << "cajeta: conformance: [" << run.results.size() << "/" << launches.size()
                   << "] " << r.launch << " " << run.results.back().outcome << " ("
                   << (int) (s * 1000) << " ms)";
            const std::string& o = run.results.back().outcome;
            if (o != "pass") (*log) << ": " << run.results.back().detail;
            (*log) << "\n";
            log->flush();
        };
        try {
            reference::run(kernel, argv, launch);
        } catch (cajeta::Exception& e) {
            r.outcome = e.getErrorId() == "XPU-REF01" ? "refused"
                      : e.getErrorId() == "XPU-REF03" ? "slow" : "undefined";
            r.detail = e.getErrorId() + ": " + e.getMessage();
            run.results.push_back(r);
            progress();
            continue;
        }

        // Compare each buffer parameter's view, element by element, at its
        // element type; an allocation two parameters share is compared once.
        Held h = heldFor(held, r.kernel, r.backend);
        std::string first;
        std::vector<bool> compared(mem.size(), false);
        for (size_t i = 0; i < shapes.size() && first.empty(); ++i) {
            if (!shapes[i].isBuffer || where[i].first < 0) continue;
            size_t al = (size_t) where[i].first;
            if (compared[al]) continue;
            compared[al] = true;
            unsigned eb = std::max(1u, shapes[i].elementBytes);
            size_t off = (size_t) where[i].second;
            const uint8_t* want = mem[al].data();
            const uint8_t* got = after[al].data();
            size_t n = (mem[al].size() - off) / eb;
            for (size_t e = 0; e < n; ++e) {
                uint64_t x = loadBits(want + off + e * eb, eb);
                uint64_t y = loadBits(got + off + e * eb, eb);
                if (x == y) continue;
                bool same = false;
                uint64_t d = 0;
                if (shapes[i].isFloat) {
                    bool nx = isNanBits(x, eb, false), ny = isNanBits(y, eb, false);
                    same = nx && ny;
                    if (!nx && !ny) {
                        d = ulps(x, y, eb);
                        same = d <= h.ulps;
                    }
                }
                if (same) continue;
                std::ostringstream os;
                os << shapes[i].name << "[" << e << "]: the reference wrote " << hex(x, eb)
                   << ", the backend " << hex(y, eb);
                if (shapes[i].isFloat && d) os << " (" << d << " ulp, bound " << h.ulps << ")";
                first = os.str();
                break;
            }
        }
        if (first.empty()) {
            r.outcome = h.held ? "stale" : "pass";
            if (h.held) r.detail = "held (" + h.note + ") but it agrees now: drop the hold";
        } else {
            r.outcome = h.held ? "held" : "fail";
            r.detail = first + (h.held ? " [held: " + h.note + "]" : "");
        }
        run.results.push_back(r);
        progress();
    }
    return run;
}

std::string toTsv(const CorpusRun& run) {
    std::ostringstream os;
    os << "kernel\tbackend\tlaunch\toutcome\tdetail\n";
    for (auto& r : run.results)
        os << r.kernel << '\t' << r.backend << '\t' << r.launch << '\t' << r.outcome << '\t'
           << r.detail << '\n';
    return os.str();
}

} // namespace reference
} // namespace xpu
} // namespace cajeta
