// The kernel conformance corpus runner (xpu-kernel-independence Unit 1, spec
// §2.1, §2.2, §2.4).
//
// A corpus is a directory of launches a backend ran, as CAJETA_XPU_RECORD
// writes them (runtime/native/cajeta_xpu_record.c): each launch's arguments,
// its allocations before the kernel ran, and the same allocations after. The
// runner replays each launch through the reference interpreter from the
// recorded inputs and compares what the interpreter writes against what the
// backend wrote: integers bit for bit, floats within the bound the held list
// states for that kernel (0 ulp when it states none).
//
// The held list (one line per kernel and backend, tab separated):
//   <kernel>  <backend>  held   <the plan item or note that tracks it>
//   <kernel>  <backend>  ulps=N <why the kernel's floats differ by up to N>
// A held kernel that disagrees is reported held, not failed; a held kernel
// that now agrees is reported stale, and fails, so the list cannot outlive
// the defect. `*` as the backend matches every backend.
#pragma once

#include <cstdint>
#include <iosfwd>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cajeta {
    class Method;
    using MethodPtr = std::shared_ptr<Method>;
}

namespace cajeta {
namespace xpu {
namespace reference {

    struct CorpusResult {
        std::string kernel;
        std::string backend;
        std::string launch;     // the recording's directory name
        // pass | fail | held | stale | refused | undefined | missing | slow
        std::string outcome;
        std::string detail;     // the first disagreement, or why it did not run
    };

    struct CorpusRun {
        std::vector<CorpusResult> results;
        // How many outcomes fail the run: fail, stale and undefined.
        size_t failures() const;
    };

    // Replay every recorded launch under `recordDir` whose kernel is among
    // `kernels` (matched by the name the kernel registers under), against
    // the held list at `heldPath` ("" for none). A recording whose kernel is
    // not among them is reported `missing`.
    // Each launch may take `budgetSeconds` of interpretation (0: no limit);
    // one that runs past it is reported `slow`, which does not fail the run
    // but is not coverage either. With `log`, one line per launch as it ends.
    CorpusRun runCorpus(const std::vector<MethodPtr>& kernels, const std::string& recordDir,
                        const std::string& heldPath, double budgetSeconds = 0,
                        std::ostream* log = nullptr);

    // The run as a table, one row per launch: kernel, backend, launch,
    // outcome, detail.
    std::string toTsv(const CorpusRun& run);

} // namespace reference
} // namespace xpu
} // namespace cajeta
