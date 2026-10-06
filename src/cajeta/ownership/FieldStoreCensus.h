#pragma once

#include <functional>
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
        std::string file;
        std::string origin;    // project, dependency or stdlib
        int column = 0;
    };

    // Walks every method body of `modules` after resolution and prints `[field-store]` lines.
    class FieldStoreCensus {
    public:
        // Reads CAJETA_FIELD_STORE_CENSUS once, unless setEnabled overrode it.
        static bool enabled();
        static void setEnabled(bool on);
        // Prints every store; with `standIns`, first instantiates each template once so unused ones are walked.
        static void run(const std::list<CajetaModulePtr>& modules, bool standIns = false);
        // True when CAJETA_FIELD_STORE_CENSUS=stdlib asks a lint to walk the stdlib as well.
        static bool includesStdlib();
        // Reports every store that breaks spec 1.2 as an error, through the active engine or by throwing.
        static void check(const std::list<CajetaModulePtr>& modules);
        // Calls `sink` once per store, deduplicated across template instantiations.
        static void walk(const std::list<CajetaModulePtr>& modules,
                         const std::function<void(const FieldStoreRecord&)>& sink);
        static const std::vector<FieldStoreRecord>& records();
        static void clear();
    };

} // namespace cajeta::ownership
