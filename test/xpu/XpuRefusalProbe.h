//
// Shared helper for suites that DELIBERATELY launch a maybe-refused kernel and
// probe the refusal (they read the compile-time `[xpu-kernel-skipped]` note, or
// the Device.launchFailures() counter, or a surviving output sentinel).
//
// Since the compiler emits Device.checkLaunch() after every `.launch()`, a
// refused launch now RAISES cajeta.xpu.XpuLaunchException at the launch site
// rather than silently no-opping. A probe must therefore CATCH it, or the throw
// aborts run(). The refusal counter is bumped BEFORE the throw, so a
// launchFailures()-delta assertion still holds across the catch, and the output
// sentinel still survives (the kernel never wrote), so a value assertion is
// unchanged. Catching is thus behaviour-preserving for every existing probe.
//
// This is the one place the probe pattern lives; reuse it wherever a suite
// launches a kernel it expects a backend to refuse.
//
#ifndef CAJETA_TEST_XPU_REFUSAL_PROBE_H
#define CAJETA_TEST_XPU_REFUSAL_PROBE_H

#include <string>

namespace cajeta_test {

// The import a probed-launch source needs so `catch (XpuLaunchException e)`
// resolves. Concatenate into the source's import block.
inline constexpr const char* kXpuLaunchExceptionImport =
    "import cajeta.xpu.XpuLaunchException;\n";

// Wrap one or more launch statements so a refusal is caught rather than
// aborting run(). `launchStmts` is cajeta source (the `.launch(...)` and any
// following `s.sync();`). Returns source; the enclosing program must import
// XpuLaunchException (see kXpuLaunchExceptionImport).
inline std::string catchRefusal(const std::string& launchStmts) {
    return std::string("        try {\n") + launchStmts +
           "        } catch (XpuLaunchException e) { }\n";
}

// Did the LOWERING refuse a kernel? The registration prints one
// `[xpu-kernel-skipped]` line per kernel with no device code, and two things
// print it: the lowering refusing a construct, and -- since 2026-09-21 -- the
// registration failing to ASSEMBLE what did lower (no ld.lld, no ptxas), which
// says "no assembler" and means "this box cannot build it". A test asserting
// that a kernel LOWERS must ignore the second, or it fails on every box
// without the vendor toolchain: six amdgpu suites did exactly that on
// Phoenix until 2026-09-25.
inline bool loweringRefused(const std::string& err) {
    size_t at = 0;
    while ((at = err.find("[xpu-kernel-skipped]", at)) != std::string::npos) {
        size_t nl = err.find('\n', at);
        std::string line = err.substr(
            at, nl == std::string::npos ? std::string::npos : nl - at);
        if (line.find("no assembler") == std::string::npos) return true;
        if (nl == std::string::npos) break;
        at = nl;
    }
    return false;
}

}  // namespace cajeta_test

#endif  // CAJETA_TEST_XPU_REFUSAL_PROBE_H
