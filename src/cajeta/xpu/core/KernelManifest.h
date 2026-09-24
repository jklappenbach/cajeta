// KernelManifest — the per-(kernel, target) record emitted beside a device
// artifact, derived from the SHIPPED bytes; an unreportable field is omitted.

#pragma once

#include "KernelAccess.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
    class Module;
    class Value;
    template <typename T, typename Inserter> class IRBuilder;
    class ConstantFolder;
    class IRBuilderDefaultInserter;
}

namespace cajeta {
    class Method;
    using MethodPtr = std::shared_ptr<Method>;
}

namespace llvm {
    class Function;
    class IRBuilderBase;
}

namespace cajeta {
namespace xpu {

    // One cooperative-matrix verb and the instruction the lowering SELECTED for it
    // on this target: a native intrinsic (`nvvm.wmma.m16n16k16.mma.row.col.bf16`,
    // `amdgcn.wmma.f32.16x16x16.bf16`, `spirv.OpCooperativeMatrixMulAddKHR`) or
    // the portable tile (`software-tile.replicated`, `software-tile.distributed`).
    // Idiom recognition is fragile, and a kernel that quietly drops off the fast
    // path is the same invisible-absence failure as a vacuous pass. Recording the
    // choice makes it a fact the manifest states, not a note that scrolled past.
    struct KernelNativeOp {
        std::string op;           // mma | load.a | load.b | load.c | store
        std::string instruction;  // as above, never empty
        bool native() const { return instruction.rfind("software-tile", 0) != 0; }
    };

    struct KernelManifest {
        // tile-manifest-v1 — the runtime refuses a schema it does not know.
        static constexpr int kSchemaVersion = 1;

        std::string kernel;           // qualified: <class canonical>.<method>
        std::string target;           // e.g. "amdgpu/gfx1151", "cpu/<host cpu>"
        std::string codeHash;         // "sha256:<64 hex>" over the artifact
        std::string compilerVersion;  // CAJETA_VERSION
        int  xpuAbiVersion = 3;       // CAJETA_XPU_ABI_VERSION at build
        bool instrumented  = false;   // completion records compiled in (Unit 8)

        std::optional<unsigned> waveWidth;
        std::optional<unsigned> vgpr;
        std::optional<unsigned> sgpr;
        std::optional<unsigned> spillBytes;      // scratch / private segment bytes
        // ptxas reports "bytes stack frame" and "bytes spill stores" separately,
        // and the difference is the whole diagnosis: a frame with ZERO spill
        // stores is a construct the backend legalized through memory, not
        // register pressure, and the two have opposite remedies. ABSENT where a
        // backend reports only one number (amdgpu's private segment) — absent is
        // not zero, and nothing may read it as a claim.
        std::optional<unsigned> spillStoreBytes;
        std::optional<unsigned> ldsStaticBytes;  // static group segment bytes
        std::optional<std::string> ldsDynamicParam;   // the sizing parameter
        // A pinned block records its residency, an argument block feasibleBlocks.
        std::optional<unsigned> threadsPerGroup;
        std::optional<unsigned> residentGroupsPerCu;  // capacity per multiprocessor
        std::vector<unsigned>   feasibleBlocks;       // empty = absent
        std::optional<std::string> occupancyLimiter;  // registers|lds|waveSlots|unknown

        // One entry per buffer-like parameter, in declaration order, classified on
        // the lowered IR; `drainsDevice` = every global write hits a constant element.
        std::vector<KernelAccessEntry> access;
        bool restartable = false;
        bool drainsDevice = false;
        // False until derived — the conservative reading, never a claim.
        bool captureSafe = false;
        // What each cooperative-matrix verb lowered to (xpu-kernel-adaptor
        // 4A.2.6). Always emitted: an EMPTY list on a kernel that should carry
        // an mma is itself the visible absence. Deduplicated, first-selection order.
        std::vector<KernelNativeOp> nativeOps;

        bool hasFootprint() const {
            return waveWidth || vgpr || sgpr || spillBytes || spillStoreBytes
                || ldsStaticBytes
                || ldsDynamicParam || threadsPerGroup || residentGroupsPerCu
                || !feasibleBlocks.empty() || occupancyLimiter;
        }
        std::string simpleName() const;   // the name after the last '.'
    };

    // "<declaring class canonical>.<method>" — the manifest's kernel key.
    std::string qualifiedKernelName(const MethodPtr& kernel);

    void applyAccess(KernelManifest& m, const KernelAccessSummary& access);

    // "sha256:<64 lowercase hex>" over `len` bytes.
    std::string sha256Hex(const uint8_t* data, std::size_t len);

    std::string compilerVersionString();

    // The tile-manifest-v1 document: 2-space pretty, deterministic key order.
    std::string toJson(const KernelManifest& m);

    // False (with `error`) unless the text is a schema-1 document with identity.
    bool fromJson(const std::string& text, KernelManifest& out,
                  std::string* error = nullptr);

    // "<simple kernel>.<target with '/'->'-'>.manifest.json", beside the artifact.
    std::string manifestFileName(const KernelManifest& m);

    // The .cja member name; qualified, since one archive holds every class's kernels.
    std::string manifestArchiveMemberName(const KernelManifest& m);

    // The section the embedded JSON lands in: ".cajeta.manifest" on ELF, ".cajmf"
    // on COFF (8-character names), "__DATA,__cajeta_mf" on Mach-O.
    const char* manifestSectionName(const llvm::Module& host);

    // Fills the occupancy fields from the arch table: a set `pinnedThreads` records
    // the pin, unset records feasibleBlocks best-first under `clamp`.
    void fillOccupancy(KernelManifest& m, const std::string& archName,
                       std::optional<unsigned> pinnedThreads, unsigned clamp = 0);

    // Warns on stderr when spillBytes > 0, so a spilling kernel is never silently
    // accepted as tuned. True if it warned.
    /** `softwareCoopTileBytes` = per-work-item bytes of portable software
     *  CooperativeMatrix storage, from the function's cajeta-cooptile-bytes
     *  attribute; 0 when the kernel has none. */
    bool warnIfSpilling(const KernelManifest& m,
                        uint64_t softwareCoopTileBytes = 0);

    // Embeds `m` as JSON in `host` and emits, at the builder's insert point in a
    // registration ctor, the __cajeta_xpu_register_kernel_manifest call that lets
    // the runtime serve `k.manifest()`. `arch` is the token, "gfx1151" or "".
    /** Stamp on the function that cooperative verb `op` lowered to `instruction`,
     *  as a `cajeta-native-ops` attribute (`op=instruction;...`, deduplicated).
     *  The registration emitter reads it back with applyNativeOps. The builder
     *  form names the function `b` is inserting into. */
    void recordNativeOp(llvm::Function* fn, const std::string& op,
                        const std::string& instruction);
    void recordNativeOp(llvm::IRBuilderBase& b, const std::string& op,
                        const std::string& instruction);
    /** Fill `m.nativeOps` from `kfn`'s cajeta-native-ops attribute. Empty when
     *  the function carries none, and never a claim beyond what was stamped. */
    void applyNativeOps(KernelManifest& m, const llvm::Function* kfn);
    /** The manifest's name for an LLVM intrinsic: its base name without the
     *  `llvm.` prefix, e.g. `nvvm.wmma.m16n16k16.mma.row.col.bf16`. */
    std::string nativeInstructionName(unsigned llvmIntrinsicId);

    void emitManifestRegistration(
        llvm::Module& host,
        llvm::IRBuilder<llvm::ConstantFolder, llvm::IRBuilderDefaultInserter>& b,
        llvm::Value* nameStr, int backendId, const std::string& arch,
        const KernelManifest& m);

} // namespace xpu
} // namespace cajeta
