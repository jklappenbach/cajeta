//
// KernelManifest — the per-(kernel, target) record the compiler emits beside a
// device artifact (specs/xpu-tile-manifest-spec.md §2, §3, §12; schema
// specs/schemas/tile-manifest-v1.schema.json).
//
// Built where the SHIPPED device code is assembled — each backend's
// registration emitter — so the hash and footprint describe the bytes that
// actually register with the runtime, never an inspection artifact of the
// same source. Every quantity here is derived from the artifact or the
// occupancy picker; nothing is author-declared (spec D6).
//
// Absence is a value: a field the target cannot report is std::nullopt and
// is NOT emitted (never zero) — the CPU backend has no VGPR count, Vulkan
// has no compile-time pipeline statistics (spec §1.4, §2.4).
//

#pragma once

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

namespace cajeta {
namespace xpu {

    struct KernelManifest {
        // tile-manifest-v1 — the runtime refuses a schema it does not know.
        static constexpr int kSchemaVersion = 1;

        // ---- identity (§2) ---------------------------------------------- //
        std::string kernel;           // qualified: <class canonical>.<method>
        std::string target;           // "amdgpu/gfx1151", "nvptx/sm_89",
                                      // "spirv/vulkan1.3", "cpu/<host cpu>"
        std::string codeHash;         // "sha256:<64 hex>" over the artifact
        std::string compilerVersion;  // CAJETA_VERSION
        int  xpuAbiVersion = 3;       // CAJETA_XPU_ABI_VERSION at build
        bool instrumented  = false;   // completion records compiled in (Unit 8)

        // ---- footprint (§3) — from the assembled artifact ----------------- //
        std::optional<unsigned> waveWidth;
        std::optional<unsigned> vgpr;
        std::optional<unsigned> sgpr;
        std::optional<unsigned> spillBytes;      // scratch / private segment bytes
        std::optional<unsigned> ldsStaticBytes;  // static group segment bytes
        std::optional<std::string> ldsDynamicParam;   // §3.5: the sizing parameter
        // §3.3: a pinned block records the pin + the picker's residency; an
        // argument block records the picker's feasible sizes, best first.
        std::optional<unsigned> threadsPerGroup;
        std::optional<unsigned> residentGroupsPerCu;  // per driver multiprocessor —
                                                      // a CAPACITY unit (§3.4)
        std::vector<unsigned>   feasibleBlocks;       // empty = absent
        std::optional<std::string> occupancyLimiter;  // registers|lds|waveSlots|unknown

        // ---- required by the schema; derived by later units ---------------- //
        // Unit 3 derives `restartable` from the access sets (§6.5); Unit 7
        // derives `captureSafe` (§8.3). Until then both are false — the
        // conservative reading, never a claim.
        bool restartable = false;
        bool captureSafe = false;

        bool hasFootprint() const {
            return waveWidth || vgpr || sgpr || spillBytes || ldsStaticBytes
                || ldsDynamicParam || threadsPerGroup || residentGroupsPerCu
                || !feasibleBlocks.empty() || occupancyLimiter;
        }
        // The kernel's simple name (after the last '.').
        std::string simpleName() const;
    };

    // "<declaring class canonical>.<method>" — the manifest's kernel key (§2.1).
    std::string qualifiedKernelName(const MethodPtr& kernel);

    // "sha256:<64 lowercase hex>" over `len` bytes.
    std::string sha256Hex(const uint8_t* data, std::size_t len);

    // The compiler's version string as stamped at configure time.
    std::string compilerVersionString();

    // Serialize to the tile-manifest-v1 JSON document (pretty, 2-space).
    // Absent optionals are omitted. Deterministic key order.
    std::string toJson(const KernelManifest& m);

    // Parse a tile-manifest-v1 document. False (with `error`) when the text is
    // not JSON, is not schema version 1, or lacks identity. Fields the document
    // omits stay absent.
    bool fromJson(const std::string& text, KernelManifest& out,
                  std::string* error = nullptr);

    // "<simple kernel>.<target with '/' -> '-'>.manifest.json" — the JSON
    // copy's file name beside the artifact (§12.4).
    std::string manifestFileName(const KernelManifest& m);

    // "xpu/manifests/<qualified kernel>.<target with '/' -> '-'>.manifest.json"
    // — the .cja member name (§12.4; CajetaArchive::EntryKind::KernelManifest).
    // Qualified, because one archive holds every class's kernels.
    std::string manifestArchiveMemberName(const KernelManifest& m);

    // The object section the embedded manifest JSON lands in, by object format
    // (§12.4 "a data section of an --emit=exe binary"): ".cajeta.manifest" on
    // ELF, ".cajmf" on COFF (8-character section names), "__DATA,__cajeta_mf"
    // on Mach-O. Concatenated NUL-terminated tile-manifest-v1 documents; a tool
    // can read them without running the program.
    const char* manifestSectionName(const llvm::Module& host);

    // Fill the §3.3 occupancy fields from the arch table for `archName`
    // (DeviceProfile::lookupArch). `pinnedThreads` set = the block is pinned
    // (an @Occupancy clamp or one constant launch block): records
    // threadsPerGroup, residentGroupsPerCu and the limiter. Unset = the block
    // is a launch argument: records feasibleBlocks best-first (candidateBlocks
    // order) under `clamp` (0 = none). An unknown arch or a missing VGPR count
    // leaves every occupancy field absent.
    void fillOccupancy(KernelManifest& m, const std::string& archName,
                       std::optional<unsigned> pinnedThreads, unsigned clamp = 0);

    // §3.2 — a spilling kernel is never silently accepted as tuned: print
    // "cajeta: warning: [xpu-kernel-spill] <kernel> on <target>: N bytes ..."
    // to stderr when spillBytes > 0. Returns true if it warned.
    bool warnIfSpilling(const KernelManifest& m);

    // Embed `m` as JSON in `host` and emit, at the builder's insert point (a
    // registration ctor), the call
    //   __cajeta_xpu_register_kernel_manifest(name, backendId, arch, json, len)
    // so the runtime can serve `k.manifest()` for the active target (§12.1).
    // `nameStr` is the kernel's simple-name C string already in the ctor;
    // `arch` is the device arch token ("gfx1151", "sm_89", "", ...).
    void emitManifestRegistration(
        llvm::Module& host,
        llvm::IRBuilder<llvm::ConstantFolder, llvm::IRBuilderDefaultInserter>& b,
        llvm::Value* nameStr, int backendId, const std::string& arch,
        const KernelManifest& m);

} // namespace xpu
} // namespace cajeta
