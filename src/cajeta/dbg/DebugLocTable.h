// Debug location table for the in-process debugger (CP2+): statement-boundary codegen
// emits `__cajeta_dbg_safepoint(loc_id)`, and this maps that dense id back to
// {file, line, col, function}. One active compile per process backs the global table.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace cajeta::dbg {

    struct DbgLoc {
        std::string file;
        int line = 0;
        int col = 0;
        std::string function;  // cajeta-mangled enclosing fn (may be empty)
    };

    class DbgLocTable {
    public:
        // Append a location and return its id. Ids are dense, sequential and NOT
        // deduplicated; after setAt() replay, appends continue past the max replayed id.
        int32_t add(const std::string& file, int line, int col,
                    const std::string& function);

        // Place `loc` at exactly `id`, growing the table with HOLES as needed (a hole is
        // a default DbgLoc that at() returns harmlessly and idsForLine() never matches).
        void setAt(int32_t id, DbgLoc loc);

        // Look up by id. Caller must pass a valid id (< size()).
        const DbgLoc& at(int32_t id) const;

        // One past the highest assigned id, holes included - the table EXTENT, not the
        // entry count. Storage is SPARSE: per-module id ranges sit megabytes apart.
        size_t size() const { return (size_t) nextId; }
        bool empty() const { return locs.empty(); }
        void clear() { locs.clear(); nextId = 0; }

        // All ids whose (file, line) match, to arm a line breakpoint; `file` is exact.
        std::vector<int32_t> idsForLine(const std::string& file, int line) const;

        // Every assigned id, ascending. THE iteration surface - never scan 0..size().
        std::vector<int32_t> assignedIds() const;

    private:
        std::unordered_map<int32_t, DbgLoc> locs;
        int32_t nextId = 0;   // max assigned id + 1
    };

    // Process-global table backing codegen's emission sites; codegen is single-threaded.
    DbgLocTable& globalDbgLocTable();

    // Loc-table sidecar: sparse-native (one line per NON-hole entry), so holes round-trip
    // for free. False means no usable sidecar, and callers fall back to compiling.
    bool writeDbgLocSidecar(const std::string& path, const DbgLocTable& table);
    bool loadDbgLocSidecar(const std::string& path, DbgLocTable& into);

} // namespace cajeta::dbg
