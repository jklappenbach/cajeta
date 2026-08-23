// Shared trace reader for the profiler tests — pulls TrackDescriptor
// {uuid, name, parent_uuid} out of an emitted .pftrace.
//
// Lives in a header because two suites need it (the GPU seam's track hierarchy
// and the sampler's per-thread tracks) and a second copy would be a second
// chance to disagree with the writer about the encoding.
//
// It reads the BYTES rather than any in-memory table. That distinction is not
// academic here: 6.1.d shipped an interning table that was entirely correct
// while the bytes it produced were unframed, and the unit test asserting the
// table passed against it.
#ifndef CAJETA_TEST_PROFILER_TRACE_READ_H
#define CAJETA_TEST_PROFILER_TRACE_READ_H

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace cajeta_test_prof {

struct TrackDesc {
    uint64_t    uuid = 0;
    uint64_t    parent = 0;
    std::string name;
};

// The runtime's own varint reader, looked up from the JIT. Passing it in rather
// than writing a second one keeps a disagreement about the encoding visible
// instead of cancelling out.
using VarintRead = uint64_t (*)(const uint8_t*, int32_t, int32_t*);

inline std::vector<TrackDesc> readTracks(const char* path, VarintRead rd) {
    std::vector<TrackDesc> out;
    if (!rd) return out;
    FILE* f = std::fopen(path, "rb");
    if (!f) return out;
    std::vector<uint8_t> b;
    uint8_t chunk[4096];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
        b.insert(b.end(), chunk, chunk + n);
    std::fclose(f);

    using Visit = std::function<void(uint32_t, uint32_t, const uint8_t*, uint64_t)>;
    std::function<void(const uint8_t*, size_t, const Visit&)> walk =
        [&](const uint8_t* p, size_t len, const Visit& visit) {
            size_t i = 0;
            while (i < len) {
                int32_t used = 0;
                uint64_t key = rd(p + i, static_cast<int32_t>(len - i), &used);
                if (used <= 0) return;
                i += static_cast<size_t>(used);
                uint32_t field = static_cast<uint32_t>(key >> 3);
                uint32_t wire  = static_cast<uint32_t>(key & 7);
                if (wire == 2) {
                    uint64_t l = rd(p + i, static_cast<int32_t>(len - i), &used);
                    if (used <= 0 || i + static_cast<size_t>(used) + l > len) return;
                    i += static_cast<size_t>(used);
                    visit(field, wire, p + i, l);
                    i += static_cast<size_t>(l);
                } else if (wire == 0) {
                    uint64_t v = rd(p + i, static_cast<int32_t>(len - i), &used);
                    if (used <= 0) return;
                    i += static_cast<size_t>(used);
                    visit(field, wire, nullptr, v);
                } else if (wire == 1) {
                    if (i + 8 > len) return;
                    i += 8;
                } else {
                    return;   // wire types the writer never emits
                }
            }
        };

    walk(b.data(), b.size(), [&](uint32_t f, uint32_t w, const uint8_t* p, uint64_t l) {
        if (f != 1 /*Trace.packet*/ || w != 2) return;
        walk(p, static_cast<size_t>(l),
             [&](uint32_t pf, uint32_t pw, const uint8_t* pp, uint64_t pl) {
                 if (pf != 60 /*track_descriptor*/ || pw != 2) return;
                 TrackDesc d;
                 walk(pp, static_cast<size_t>(pl),
                      [&](uint32_t df, uint32_t dw, const uint8_t* dp, uint64_t dl) {
                          if (df == 1 && dw == 0) d.uuid = dl;
                          else if (df == 2 && dw == 2)
                              d.name.assign(reinterpret_cast<const char*>(dp), dl);
                          else if (df == 5 && dw == 0) d.parent = dl;
                      });
                 out.push_back(d);
             });
    });
    return out;
}

} // namespace cajeta_test_prof

#endif
