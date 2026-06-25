//
// XPU address-space enumeration and per-backend lowering tables.
//
// Per CajetaXPU.md §3.1.2 there are five address spaces:
//
//     Generic   — flat / generic addressing
//     Global    — device global memory
//     Shared    — workgroup-shared memory
//     Constant  — read-only uniform
//     Private   — per-thread private
//
// Each backend maps these to its own integer/storage-class
// representation. The table below is the literal artifact behind
// CajetaXPU-Variance.md row 4 (atomic scopes) and row 5 (memory
// model) — adding AMD/Vulkan later only fills in the unfilled
// columns of `numberFor()`.
//

#pragma once

#include <cstdint>
#include <string_view>

namespace cajeta {
namespace xpu {

    enum class AddressSpace : uint8_t {
        Generic  = 0,
        Global   = 1,
        Shared   = 2,
        Constant = 3,
        Private  = 4,
    };

    // Human-readable name for diagnostics and the MIR printer.
    constexpr std::string_view addressSpaceName(AddressSpace as) {
        switch (as) {
            case AddressSpace::Generic:  return "generic";
            case AddressSpace::Global:   return "global";
            case AddressSpace::Shared:   return "shared";
            case AddressSpace::Constant: return "constant";
            case AddressSpace::Private:  return "private";
        }
        return "?";
    }

    // Per-backend address-space numbers (CajetaXPU.md §3.1.2 table).
    // NVIDIA and AMD share the LLVM address-space convention here;
    // Vulkan/SPIR-V uses storage classes that are name-based not
    // numeric — for SPIR-V we map onto OpVariable storage classes
    // later in the Vulkan lowering pass; `spirvNumberFor` isn't a
    // real LLVM address space, just a stable integer for tests
    // (matches the SPIR-V enum value of the storage class).
    constexpr int nvidiaNumberFor(AddressSpace as) {
        switch (as) {
            case AddressSpace::Generic:  return 0;
            case AddressSpace::Global:   return 1;
            case AddressSpace::Shared:   return 3;
            case AddressSpace::Constant: return 4;
            case AddressSpace::Private:  return 5;
        }
        return 0;
    }

    constexpr int amdNumberFor(AddressSpace as) {
        // AMDGPU agrees with NVPTX on these positions per CajetaXPU.md
        // §3.1.2; if a future GFX changes them the table forks here.
        switch (as) {
            case AddressSpace::Generic:  return 0;
            case AddressSpace::Global:   return 1;
            case AddressSpace::Shared:   return 3;
            case AddressSpace::Constant: return 4;
            case AddressSpace::Private:  return 5;
        }
        return 0;
    }

    // SPIR-V storage-class numeric values, per the Khronos spec.
    // Generic→8 (Function generic), Global→12 (StorageBuffer),
    // Shared→4 (Workgroup), Constant→2 (Uniform), Private→7 (Function).
    constexpr int spirvNumberFor(AddressSpace as) {
        switch (as) {
            case AddressSpace::Generic:  return 8;
            case AddressSpace::Global:   return 12;
            case AddressSpace::Shared:   return 4;
            case AddressSpace::Constant: return 2;
            case AddressSpace::Private:  return 7;
        }
        return 0;
    }

    // Recognition: which AddressSpace does a Cajeta canonical name
    // refer to? Returns nullopt for any canonical not in the
    // cajeta.xpu.{Global,Shared,Constant,Private,Generic} set.
    // Matches just the short class name suffix to admit both the
    // template-form ("cajeta.xpu.Global") and an instantiated
    // form ("cajeta.xpu.Global<cajeta.float32>").
    inline bool isAddressSpaceCanonical(const std::string& canonical,
                                        AddressSpace& outAs) {
        // First find where the template-arg list starts (if any).
        // Then find the last '.' BEFORE that point — find_last_of('.')
        // on the full string would land inside template args for a
        // qualified-type arg like Shared<cajeta.float32>.
        auto angle = canonical.find('<');
        auto searchEnd = (angle == std::string::npos)
            ? canonical.size() : angle;
        auto dotPos = canonical.rfind('.', searchEnd - 1);
        if (dotPos == std::string::npos) return false;
        std::string pkg = canonical.substr(0, dotPos);
        if (pkg != "cajeta.xpu") return false;
        std::string shortName = canonical.substr(
            dotPos + 1, searchEnd - dotPos - 1);
        if (shortName == "Global")   { outAs = AddressSpace::Global;   return true; }
        if (shortName == "Shared")   { outAs = AddressSpace::Shared;   return true; }
        if (shortName == "Constant") { outAs = AddressSpace::Constant; return true; }
        if (shortName == "Private")  { outAs = AddressSpace::Private;  return true; }
        if (shortName == "Generic")  { outAs = AddressSpace::Generic;  return true; }
        return false;
    }

} // namespace xpu
} // namespace cajeta
