#include "cajeta/prof/TraceSummary.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

// The runtime's own varint decoder, linked into this binary. Reused rather than
// reimplemented: a second decoder is a second chance to disagree with the
// writer about the encoding, and the writer is the only authority on it.
extern "C" uint64_t __cajeta_pb_varint_read(const uint8_t* in, int32_t max,
                                            int32_t* consumed);

namespace cajeta::prof {

    namespace {

        // Perfetto field numbers, as the writer emits them
        // (runtime/native/cajeta_rt_prof_trace.c).
        constexpr uint32_t kTracePacket      = 1;
        constexpr uint32_t kPktTimestamp     = 8;
        constexpr uint32_t kPktTrackEvent    = 11;
        constexpr uint32_t kPktInterned      = 12;
        constexpr uint32_t kPktTrackDesc     = 60;
        constexpr uint32_t kDescUuid         = 1;
        constexpr uint32_t kDescName         = 2;
        constexpr uint32_t kInternedNames    = 2;
        constexpr uint32_t kNameIid          = 1;
        constexpr uint32_t kNameStr          = 2;
        constexpr uint32_t kTeType           = 9;
        constexpr uint32_t kTeNameIid        = 10;
        constexpr uint32_t kTeTrackUuid      = 11;
        constexpr uint32_t kTeName           = 23;
        constexpr uint32_t kTeDebugAnnos     = 4;
        constexpr uint32_t kDaName           = 10;
        constexpr uint32_t kDaIntValue       = 4;
        constexpr int32_t  kSliceBegin       = 1;
        constexpr int32_t  kSliceEnd         = 2;
        // The runtime's run-metadata instant (cajeta_rt_prof_trace.c,
        // __cajeta_prof_trace_metadata): its debug annotations carry the
        // device capture ring's kept / dropped counts.
        constexpr const char* kRunMetaEvent  = "cajeta.profiler.run";

        struct Field {
            uint32_t       number = 0;
            uint32_t       wire = 0;
            const uint8_t* data = nullptr;   // wire 2
            uint64_t       len = 0;          // wire 2 length, or wire 0 value
        };

        // One level of a length-delimited message. Returns false on a malformed
        // buffer rather than throwing: a truncated trace is a normal thing to
        // be handed (a killed run still leaves a readable prefix), and the
        // caller reports what it managed to read.
        bool walk(const uint8_t* p, size_t len,
                  const std::function<void(const Field&)>& visit) {
            size_t i = 0;
            while (i < len) {
                int32_t used = 0;
                uint64_t key = __cajeta_pb_varint_read(p + i, (int32_t) (len - i), &used);
                if (used <= 0) return false;
                i += (size_t) used;
                Field f;
                f.number = (uint32_t) (key >> 3);
                f.wire = (uint32_t) (key & 7);
                if (f.wire == 2) {
                    uint64_t l = __cajeta_pb_varint_read(p + i, (int32_t) (len - i), &used);
                    if (used <= 0) return false;
                    i += (size_t) used;
                    if (l > len - i) return false;
                    f.data = p + i;
                    f.len = l;
                    visit(f);
                    i += (size_t) l;
                } else if (f.wire == 0) {
                    uint64_t v = __cajeta_pb_varint_read(p + i, (int32_t) (len - i), &used);
                    if (used <= 0) return false;
                    i += (size_t) used;
                    f.len = v;
                    visit(f);
                } else if (f.wire == 1) {
                    if (i + 8 > len) return false;
                    i += 8;
                } else if (f.wire == 5) {
                    if (i + 4 > len) return false;
                    i += 4;
                } else {
                    return false;   // wire types the writer never emits
                }
            }
            return true;
        }

        // The same rule the IDE applies (ProfileViewModel.TrackKind.of), so the
        // CLI and the tool window never disagree about what is device work.
        bool isDeviceQueue(const std::string& name) {
            return name.rfind("queue ", 0) == 0;
        }
        bool isHostLane(const std::string& name) {
            return name.rfind("cajeta.thread.", 0) == 0
                || name.rfind("cajeta.fiber.", 0) == 0;
        }

        struct Open {
            std::string name;
            int64_t     ts = 0;
            /** Inclusive time held by this frame's direct children, so its own
             *  exclusive time can be found when it closes. */
            int64_t     childNs = 0;
        };

    } // namespace

    bool summarize(const std::string& path, const SummaryOptions& opts,
                   Summary* out, std::string* err) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) {
            if (err) *err = "cannot open " + path;
            return false;
        }
        std::vector<uint8_t> buf;
        uint8_t chunk[65536];
        size_t n;
        while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
            buf.insert(buf.end(), chunk, chunk + n);
        std::fclose(f);
        if (buf.empty()) {
            if (err) *err = path + " is empty";
            return false;
        }

        std::unordered_map<uint64_t, std::string> trackName;
        std::unordered_map<uint64_t, std::string> internedName;

        // Pass 1: descriptors and interned names. Two passes because a trace
        // may name a track after the first event on it, and a one-pass reader
        // would attribute those events to an unknown track.
        walk(buf.data(), buf.size(), [&](const Field& pkt) {
            if (pkt.number != kTracePacket || pkt.wire != 2) return;
            walk(pkt.data, (size_t) pkt.len, [&](const Field& g) {
                if (g.number == kPktTrackDesc && g.wire == 2) {
                    uint64_t uuid = 0;
                    std::string name;
                    walk(g.data, (size_t) g.len, [&](const Field& d) {
                        if (d.number == kDescUuid && d.wire == 0) uuid = d.len;
                        else if (d.number == kDescName && d.wire == 2)
                            name.assign((const char*) d.data, (size_t) d.len);
                    });
                    if (uuid) trackName[uuid] = name;
                } else if (g.number == kPktInterned && g.wire == 2) {
                    walk(g.data, (size_t) g.len, [&](const Field& id) {
                        if (id.number != kInternedNames || id.wire != 2) return;
                        uint64_t iid = 0;
                        std::string name;
                        walk(id.data, (size_t) id.len, [&](const Field& e) {
                            if (e.number == kNameIid && e.wire == 0) iid = e.len;
                            else if (e.number == kNameStr && e.wire == 2)
                                name.assign((const char*) e.data, (size_t) e.len);
                        });
                        if (iid) internedName[iid] = name;
                    });
                } else if (g.number == kPktTrackEvent && g.wire == 2) {
                    // The run-metadata instant carries the device capture
                    // ring's accounting as debug annotations. Read here, in
                    // the descriptor pass, so a trace with no device slice at
                    // all still reports what its ring did.
                    std::string name;
                    std::vector<std::pair<std::string, int64_t>> annos;
                    walk(g.data, (size_t) g.len, [&](const Field& t) {
                        if (t.number == kTeName && t.wire == 2) {
                            name.assign((const char*) t.data, (size_t) t.len);
                        } else if (t.number == kTeDebugAnnos && t.wire == 2) {
                            std::string key;
                            int64_t v = 0;
                            bool hasV = false;
                            walk(t.data, (size_t) t.len, [&](const Field& d) {
                                if (d.number == kDaName && d.wire == 2)
                                    key.assign((const char*) d.data, (size_t) d.len);
                                else if (d.number == kDaIntValue && d.wire == 0) {
                                    v = (int64_t) d.len;
                                    hasV = true;
                                }
                            });
                            if (hasV) annos.emplace_back(key, v);
                        }
                    });
                    if (name == kRunMetaEvent) {
                        for (const auto& a : annos) {
                            if (a.first == "gpu_records_kept") out->gpuRecordsKept = a.second;
                            else if (a.first == "gpu_records_dropped") out->gpuRecordsDropped = a.second;
                        }
                    }
                }
            });
        });

        // Pass 2: pair BEGIN with END per track. The writer emits boundaries,
        // not durations — the same reason trace_processor computes `dur` by
        // nesting rather than reading it.
        std::unordered_map<uint64_t, std::vector<Open>> stacks;
        struct Rec { std::string name; int64_t ts; int64_t dur; int64_t self; };
        std::vector<Rec> recs;
        int64_t tracks = 0;
        bool sawTrack = false;

        for (const auto& kv : trackName) {
            const bool want = opts.host ? isHostLane(kv.second)
                                        : isDeviceQueue(kv.second);
            if (want) { tracks++; sawTrack = true; }
        }

        walk(buf.data(), buf.size(), [&](const Field& pkt) {
            if (pkt.number != kTracePacket || pkt.wire != 2) return;
            int64_t ts = 0;
            const uint8_t* te = nullptr;
            uint64_t teLen = 0;
            walk(pkt.data, (size_t) pkt.len, [&](const Field& g) {
                if (g.number == kPktTimestamp && g.wire == 0) ts = (int64_t) g.len;
                else if (g.number == kPktTrackEvent && g.wire == 2) {
                    te = g.data; teLen = g.len;
                }
            });
            if (!te) return;
            int32_t type = 0;
            uint64_t uuid = 0;
            std::string name;
            walk(te, (size_t) teLen, [&](const Field& t) {
                if (t.number == kTeType && t.wire == 0) type = (int32_t) t.len;
                else if (t.number == kTeTrackUuid && t.wire == 0) uuid = t.len;
                else if (t.number == kTeNameIid && t.wire == 0) {
                    auto it = internedName.find(t.len);
                    if (it != internedName.end()) name = it->second;
                } else if (t.number == kTeName && t.wire == 2) {
                    name.assign((const char*) t.data, (size_t) t.len);
                }
            });
            auto tn = trackName.find(uuid);
            if (tn == trackName.end()) return;
            const bool want = opts.host ? isHostLane(tn->second)
                                        : isDeviceQueue(tn->second);
            if (!want) return;

            if (type == kSliceBegin) {
                stacks[uuid].push_back(Open{name, ts});
            } else if (type == kSliceEnd) {
                auto& st = stacks[uuid];
                if (st.empty()) return;          // an END with nothing open —
                                                 // a trace that began mid-slice
                Open o = st.back();
                st.pop_back();
                const int64_t dur = ts - o.ts;
                // Clamped: an unmatched child or a clock that stepped could
                // otherwise make a frame's self time negative, which is not a
                // duration and would corrupt the totals it feeds.
                int64_t self = dur - o.childNs;
                if (self < 0) self = 0;
                recs.push_back(Rec{o.name, o.ts, dur, self});
                if (!st.empty()) st.back().childNs += dur;
            }
        });

        out->trackCount = tracks;
        out->sawAnyTrack = sawTrack;
        if (recs.empty()) return true;

        // The window anchors on the FIRST matching slice, not on absolute
        // timestamps: those are host-clock nanoseconds and differ every run, so
        // an absolute window cannot be reused across two runs of one program.
        int64_t t0 = recs.front().ts;
        for (const auto& r : recs) t0 = std::min(t0, r.ts);

        std::map<std::string, KernelStat> byName;
        int64_t lo = 0, hi = 0;
        bool first = true;
        for (const auto& r : recs) {
            const int64_t rel = r.ts - t0;
            if (rel < opts.fromNs) continue;
            if (opts.toNs >= 0 && rel > opts.toNs) continue;
            auto& k = byName[r.name];
            k.name = r.name;
            k.count++;
            k.totalNs += r.dur;
            k.selfNs += r.self;
            k.maxNs = std::max(k.maxNs, r.dur);
            out->sliceCount++;
            if (first) { lo = hi = r.ts; first = false; }
            lo = std::min(lo, r.ts);
            hi = std::max(hi, r.ts + r.dur);
        }
        out->spanNs = first ? 0 : hi - lo;
        for (auto& kv : byName) {
            out->totalSelfNs += kv.second.selfNs;
            out->rows.push_back(kv.second);
        }
        // Ordered by SELF time: "where did the time go" is answered by the work
        // a frame did itself, not by how much of the run it happened to span.
        // On a device view self == total, so the device ordering is unchanged.
        std::sort(out->rows.begin(), out->rows.end(),
                  [](const KernelStat& a, const KernelStat& b) {
                      if (a.selfNs != b.selfNs) return a.selfNs > b.selfNs;
                      if (a.totalNs != b.totalNs) return a.totalNs > b.totalNs;
                      return a.name < b.name;   // stable, so output is diffable
                  });
        return true;
    }

} // namespace cajeta::prof

namespace cajeta::prof {

    namespace {

        // "20ms", "500us", "1s", or a bare nanosecond count. A bare number is
        // ns because that is the trace's own unit; every suffix is spelled out
        // so a reader never has to guess whether `20` meant ms.
        bool parseDuration(const std::string& s, int64_t* out) {
            if (s.empty()) return false;
            size_t i = 0;
            while (i < s.size() && (std::isdigit((unsigned char) s[i]) || s[i] == '-')) i++;
            if (i == 0) return false;
            const int64_t v = std::strtoll(s.substr(0, i).c_str(), nullptr, 10);
            const std::string unit = s.substr(i);
            if (unit.empty() || unit == "ns") { *out = v; return true; }
            if (unit == "us") { *out = v * 1000; return true; }
            if (unit == "ms") { *out = v * 1000000; return true; }
            if (unit == "s")  { *out = v * 1000000000LL; return true; }
            return false;
        }

        std::string fmtNs(int64_t ns) {
            char b[64];
            if (ns >= 1000000000LL) std::snprintf(b, sizeof(b), "%.2f s", ns / 1e9);
            else if (ns >= 1000000) std::snprintf(b, sizeof(b), "%.2f ms", ns / 1e6);
            else if (ns >= 1000)    std::snprintf(b, sizeof(b), "%.2f us", ns / 1e3);
            else                    std::snprintf(b, sizeof(b), "%" PRId64 " ns", ns);
            return b;
        }

        int usage() {
            std::fprintf(stderr,
                "Usage: cajeta profile summary <trace.pftrace> [options]\n"
                "\n"
                "Per-kernel totals from a profiled run — the question the tool\n"
                "window answers, without opening it.\n"
                "\n"
                "  --from=<dur>   window start, relative to the first slice\n"
                "  --to=<dur>     window end (default: the whole run)\n"
                "  --host         total HOST frames instead of device kernels\n"
                "  --csv          machine-readable output; its first line is\n"
                "                 `# gpu_records_kept=N gpu_records_dropped=M` when\n"
                "                 the trace carries the device ring's accounting\n"
                "\n"
                "<dur> is ns unless suffixed: 500us, 20ms, 1s.\n"
                "The window is RELATIVE because absolute timestamps are\n"
                "host-clock nanoseconds and differ on every run.\n");
            return 2;
        }

    } // namespace

    int dispatchProfile(int argc, const char* const* argv) {
        if (argc < 3) return usage();
        const std::string verb = argv[2];
        if (verb != "summary") {
            std::fprintf(stderr, "cajeta profile: unknown subcommand '%s'\n", verb.c_str());
            return usage();
        }
        std::string path;
        SummaryOptions opts;
        bool csv = false;
        for (int i = 3; i < argc; i++) {
            const std::string a = argv[i];
            if (a.rfind("--from=", 0) == 0) {
                if (!parseDuration(a.substr(7), &opts.fromNs)) {
                    std::fprintf(stderr, "cajeta profile: bad --from: %s\n", a.c_str() + 7);
                    return 2;
                }
            } else if (a.rfind("--to=", 0) == 0) {
                if (!parseDuration(a.substr(5), &opts.toNs)) {
                    std::fprintf(stderr, "cajeta profile: bad --to: %s\n", a.c_str() + 5);
                    return 2;
                }
            } else if (a == "--host") {
                opts.host = true;
            } else if (a == "--csv") {
                csv = true;
            } else if (a == "--help" || a == "-h") {
                return usage();
            } else if (!a.empty() && a[0] == '-') {
                std::fprintf(stderr, "cajeta profile: unknown option: %s\n", a.c_str());
                return 2;
            } else if (path.empty()) {
                path = a;
            } else {
                std::fprintf(stderr, "cajeta profile: unexpected argument: %s\n", a.c_str());
                return 2;
            }
        }
        if (path.empty()) return usage();

        Summary sum;
        std::string err;
        if (!summarize(path, opts, &sum, &err)) {
            std::fprintf(stderr, "cajeta profile: %s\n", err.c_str());
            return 1;
        }

        const char* what = opts.host ? "host frames" : "kernels";
        if (!sum.sawAnyTrack) {
            // Distinct from "the window excluded everything": one is a profile
            // of a run that never touched the device, the other is a bad
            // window. Printing an empty table for both would conflate them.
            std::fprintf(stderr,
                "cajeta profile: no %s in %s — this run has no %s track\n",
                what, path.c_str(), opts.host ? "host" : "device queue");
            return 1;
        }
        if (sum.rows.empty()) {
            std::fprintf(stderr,
                "cajeta profile: %" PRId64 " matching track(s), but no slice fell in the window\n",
                sum.trackCount);
            return 1;
        }

        if (csv) {
            // The ring's accounting first, as a comment line, so a consumer
            // can refuse totals from a lossy ring before it reads a number.
            if (sum.gpuRecordsKept >= 0) {
                std::printf("# gpu_records_kept=%" PRId64 " gpu_records_dropped=%" PRId64 "\n",
                            sum.gpuRecordsKept, sum.gpuRecordsDropped);
            }
            std::printf("name,count,total_ns,self_ns,avg_ns,max_ns\n");
            for (const auto& r : sum.rows) {
                std::printf("%s,%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 "\n",
                            r.name.c_str(), r.count, r.totalNs, r.selfNs,
                            r.avgNs(), r.maxNs);
            }
            return 0;
        }

        size_t w = 6;
        for (const auto& r : sum.rows) w = std::max(w, r.name.size());
        const char* head = opts.host ? "frame" : "kernel";
        std::printf("%-*s %8s %12s %12s %12s %12s %7s\n", (int) w, head,
                    "count", "self", "total", "avg", "max", "share");
        // Share is of SELF time. Sharing out inclusive time would give a host
        // table whose column sums to more than the run is long, and percentages
        // of a number that does not exist.
        for (const auto& r : sum.rows) {
            std::printf("%-*s %8" PRId64 " %12s %12s %12s %12s %6.1f%%\n",
                        (int) w, r.name.c_str(), r.count,
                        fmtNs(r.selfNs).c_str(), fmtNs(r.totalNs).c_str(),
                        fmtNs(r.avgNs()).c_str(), fmtNs(r.maxNs).c_str(),
                        sum.totalSelfNs > 0
                            ? (100.0 * (double) r.selfNs / (double) sum.totalSelfNs)
                            : 0.0);
        }
        // `self` sums honestly; `total` does not, because host frames nest.
        // Both are printed, and the footer totals the one that is a duration.
        // It can still EXCEED the wall span when tracks run concurrently, which
        // is why a single "utilisation" figure is not offered.
        std::printf("\n%" PRId64 " slice(s) over %" PRId64 " track(s); "
                    "self %s, wall %s\n",
                    sum.sliceCount, sum.trackCount,
                    fmtNs(sum.totalSelfNs).c_str(), fmtNs(sum.spanNs).c_str());
        if (opts.host) {
            std::printf("`total` counts a frame's children too, so that column "
                        "sums to more than the run is long.\n");
        }
        if (sum.gpuRecordsKept >= 0) {
            const int64_t total = sum.gpuRecordsKept + sum.gpuRecordsDropped;
            std::printf("device records: %" PRId64 " kept, %" PRId64 " dropped (%" PRId64 " per mille)\n",
                        sum.gpuRecordsKept, sum.gpuRecordsDropped,
                        total > 0 ? (sum.gpuRecordsDropped * 1000) / total : 0);
            if (sum.gpuRecordsDropped > 0) {
                std::printf("the capture ring overwrote its oldest records: averages stand, "
                            "totals and shares are over the kept tail — "
                            "set CAJETA_PROFILER_GPU_RING above the launch count\n");
            }
        }
        return 0;
    }

} // namespace cajeta::prof
