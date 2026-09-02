// `cajeta profile summary` — per-kernel totals from a .pftrace, without the IDE.
//
// The common question about a GPU run is "which kernel cost what", and that is
// a QUERY, not a picture. It was answerable only by opening the tool window or
// by hand-writing SQL against Perfetto's trace_processor_shell — which is a
// separate download the toolchain does not ship (cajeta-profiler 14.4).
//
// The trace is read directly for that reason: requiring trace_processor_shell
// would move the dependency rather than remove it.
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
        /**
         * Exclusive: inclusive minus the time its direct children held.
         *
         * Host frames NEST — `run` contains `runStream` contains `upload` — so
         * an inclusive column sums parent and child together and double-counts.
         * A host table summed to 563 ms of a 276 ms run before this existed.
         * Device queues do not nest, so a kernel's self equals its total, which
         * is asserted rather than assumed.
         */
        int64_t     selfNs = 0;
        int64_t     maxNs = 0;
        int64_t avgNs() const { return count > 0 ? totalNs / count : 0; }
        int64_t avgSelfNs() const { return count > 0 ? selfNs / count : 0; }
    };

    struct SummaryOptions {
        /** Window bounds in ns, RELATIVE to the first device slice. Absolute
         *  timestamps are host-clock nanoseconds and differ every run, so an
         *  absolute window is not reusable between two runs of one program. */
        int64_t fromNs = 0;
        /** Negative means unbounded. */
        int64_t toNs = -1;
        /** Report host frames instead of device work. Off by default: a
         *  per-kernel table that silently summed host frames in beside the
         *  kernels would be the obvious wrong answer. */
        bool host = false;
    };

    struct Summary {
        std::vector<KernelStat> rows;      // sorted by totalNs, descending
        int64_t sliceCount = 0;
        /** Sum of `selfNs` over every row: the real busy time, no double
         *  counting. May still exceed `spanNs` when tracks run concurrently. */
        int64_t totalSelfNs = 0;
        int64_t spanNs = 0;                // first to last slice in the window
        int64_t trackCount = 0;            // matching tracks in the trace
        bool    sawAnyTrack = false;       // a trace with no matching track at
                                           // all is a different failure from
                                           // one whose window excluded them
    };

    /**
     * Total per kernel. Returns false and sets `err` when the file cannot be
     * read; an empty result with `sawAnyTrack == false` is not an error — it is
     * a CPU-only profile, and the caller says so rather than printing an empty
     * table.
     */
    bool summarize(const std::string& path, const SummaryOptions& opts,
                   Summary* out, std::string* err);

    /** `cajeta profile ...`. Returns a process exit code. */
    int dispatchProfile(int argc, const char* const* argv);

} // namespace cajeta::prof
