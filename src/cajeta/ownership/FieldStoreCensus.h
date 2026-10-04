#pragma once

#include <list>
#include <string>
#include <vector>

#include "../compile/CajetaModule.h"

namespace cajeta::ownership {

    // One field or slot store of a name or an interior read (spec field-store-ownership 1.2.1).
    struct FieldStoreRecord {
        std::string className;
        std::string methodName;
        int line = 0;
        std::string target;    // field, nested, slot, static, holder-local
        std::string op;        // `=`, `#=`, `=#` (legacy `= #x`)
        std::string source;    // formal-plain, formal-sharp, local-heap, local-call, ...
        std::string name;      // the stored name, or the root of an interior read
        std::string type;      // declared type of the stored name, `?` when unknown
    };

    // Walks every method body of `modules` after resolution and prints `[field-store]` lines.
    class FieldStoreCensus {
    public:
        // Reads CAJETA_FIELD_STORE_CENSUS once, unless setEnabled overrode it.
        static bool enabled();
        static void setEnabled(bool on);
        static void run(const std::list<CajetaModulePtr>& modules);
        static const std::vector<FieldStoreRecord>& records();
        static void clear();
    };

} // namespace cajeta::ownership
