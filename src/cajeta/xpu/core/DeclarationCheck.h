// A kernel's declarations, checked against what the backend produced
// (xpu-kernel-independence 4.2.2.1, spec §2.3).
//
// @Occupancy and @Wave(width) were once parsed and then ignored: the bound
// reached the IR and the PTX printer dropped it (cajeta a538aa77's shape),
// and nothing said so. Each declaration is now checked after the backend has
// spoken, against the backend's own output rather than the IR it was asked
// for: the PTX entry header for a thread bound, the manifest's wave width for
// a declared width. A declaration that did not land is an error naming the
// kernel, the backend and the declaration, never a note.
//
// The test-only lever CAJETA_XPU_FAULT=drop-occupancy makes the nvptx
// lowering lose the bound again, and drop-wave-pin makes the cpu lowering
// skip the wave pin, so the check has a test that it fires beside the one
// that it does not (XpuDeclarationCheckTests).

#pragma once

#include "cajeta/xpu/core/XpuKernelAttr.h"

#include <string>

namespace cajeta {
namespace xpu {

    // The @Occupancy declarations of `entryName` against its PTX: each one
    // the kernel made must appear as its directive (.maxntid, .minnctapersm,
    // .maxnreg) in the entry's header. Throws CAJETA_ERROR_XPU_DECLARATION_DROPPED
    // naming the kernel and the directive when one is missing.
    void checkOccupancyInPtx(const XpuKernelAttr& attr, const std::string& entryName,
                             const std::string& ptx);

    // A declared @Wave(width = N) against the width the backend built the
    // kernel at, as its manifest records it. A kernel built at another width
    // is CAJETA_ERROR_XPU_DECLARATION_DROPPED. A kernel the backend built with
    // no wave at all (width 0 or 1: no wave operation in it) is left alone,
    // since the declaration binds nothing there.
    void checkWaveWidthBuilt(const XpuKernelAttr& attr, const std::string& entryName,
                             const std::string& backend, unsigned builtWidth);

    // True when the test-only lever CAJETA_XPU_FAULT names `fault`.
    bool xpuFault(const char* fault);

} // namespace xpu
} // namespace cajeta
