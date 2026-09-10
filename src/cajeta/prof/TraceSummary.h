// `cajeta profile summary` - per-kernel totals read straight from a .pftrace.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cajeta::prof {

    /** One kernel name's totals across every device queue in the window. */
    struct KernelStat {
        std::string name;
        int64_t     count = 0;
        /** Inclusive: the frame's own span, children included. */
        int64_t     totalNs = 0;
        /** Exclusive: inclusive minus the time direct children held. Host frames NEST
         *  so inclusive double-counts; device queues do not, and that is asserted. */
        int64_t     selfNs = 0;
        int64_t     maxNs = 0;
        int64_t avgNs() const { return count > 0 ? totalNs / count : 0; }
        int64_t avgSelfNs() const { return count > 0 ? selfNs / count : 0; }
    };

    struct SummaryOptions {
        /** Window bounds in ns, RELATIVE to the first device slice: absolute
         *  timestamps are host-clock ns and differ every run. */
        int64_t fromNs = 0;
        /** Negative means unbounded. */
        int64_t toNs = -1;
        /** Report host frames instead of device work; off by default so a per-kernel
         *  table never silently sums host frames in beside the kernels. */
        bool host = false;
    };

    struct Summary {
        std::vector<KernelStat> rows;      // sorted by totalNs, descending
        int64_t sliceCount = 0;
        /** Sum of `selfNs` over every row; may exceed `spanNs` on concurrent tracks. */
        int64_t totalSelfNs = 0;
        int64_t spanNs = 0;                // first to last slice in the window
        int64_t trackCount = 0;            // matching tracks in the trace
        bool    sawAnyTrack = false;       // no track at all != a window that excluded
        /** The device CAPTURE ring's own accounting. It overwrites its oldest records,
         *  so a lossy run keeps the tail: averages survive, totals do not. -1 = unknown. */
        int64_t gpuRecordsKept = -1;
        int64_t gpuRecordsDropped = -1;
    };

    /** Total per kernel. False with `err` set when the file cannot be read; an empty
     *  result with `sawAnyTrack == false` is a CPU-only profile, not an error. */
    bool summarize(const std::string& path, const SummaryOptions& opts,
                   Summary* out, std::string* err);

    /** `cajeta profile ...`. Returns a process exit code. */
    int dispatchProfile(int argc, const char* const* argv);

} // namespace cajeta::prof
