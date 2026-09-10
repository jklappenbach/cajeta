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

namespace cajeta {
namespace xpu {

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

        bool hasFootprint() const {
            return waveWidth || vgpr || sgpr || spillBytes || ldsStaticBytes
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
    bool warnIfSpilling(const KernelManifest& m);

    // Embeds `m` as JSON in `host` and emits, at the builder's insert point in a
    // registration ctor, the __cajeta_xpu_register_kernel_manifest call that lets
    // the runtime serve `k.manifest()`. `arch` is the token, "gfx1151" or "".
    void emitManifestRegistration(
        llvm::Module& host,
        llvm::IRBuilder<llvm::ConstantFolder, llvm::IRBuilderDefaultInserter>& b,
        llvm::Value* nameStr, int backendId, const std::string& arch,
        const KernelManifest& m);

} // namespace xpu
} // namespace cajeta
