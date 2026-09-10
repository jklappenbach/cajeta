// XPU address-space enumeration and per-backend lowering tables. CajetaXPU.md §3.1.2
// defines five spaces (Generic, Global, Shared, Constant, Private); each backend maps
// them to its own integer or storage-class representation in the tables below.
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

    // Per-backend address-space numbers (CajetaXPU.md §3.1.2). NVIDIA and AMD share the
    // LLVM convention; `spirvNumberFor` is a storage class, not a real address space.
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
        // AMDGPU agrees with NVPTX on these positions; a future GFX forks the table here.
        switch (as) {
            case AddressSpace::Generic:  return 0;
            case AddressSpace::Global:   return 1;
            case AddressSpace::Shared:   return 3;
            case AddressSpace::Constant: return 4;
            case AddressSpace::Private:  return 5;
        }
        return 0;
    }

    // SPIR-V storage-class values per Khronos: Generic 8 (Function generic), Global 12
    // (StorageBuffer), Shared 4 (Workgroup), Constant 2 (Uniform), Private 7 (Function).
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

    // Which AddressSpace a Cajeta canonical names; false for any outside cajeta.xpu.
    // Matches the short class name, so both `Global` and `Global<T>` are recognised.
    inline bool isAddressSpaceCanonical(const std::string& canonical,
                                        AddressSpace& outAs) {
        // Find the last '.' BEFORE the template-arg list: find_last_of('.') on the whole
        // string lands inside the args for a qualified type like Shared<cajeta.float32>.
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
