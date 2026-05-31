//
// Debug location table for the in-process debugger (CP2+).
//
// When `--debug-info` is on, statement-boundary codegen emits a call to
// `__cajeta_dbg_safepoint(loc_id)` before each statement. `loc_id` is a dense
// small integer assigned here; this table maps it back to the source position
// `{file, line, col, function}`. CP3 projects an armed-breakpoint bitset over
// these ids; CP4's `stackTrace`/`scopes` read file+line from them.
//
// There is one active compile per process during codegen (single-threaded), so
// a process-global table (globalDbgLocTable) backs the emission sites. Call
// clear() at the start of a debug compile to drop a previous run's entries.
//
#pragma once

#include <cstdint>
#include <string>
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
        // Append a location and return its id. Ids are dense and sequential
        // (0, 1, 2, ...), one per emission site (one per statement) — NOT
        // deduplicated, so two statements on the same line get distinct ids
        // (both map to the same (file,line) for breakpoint arming).
        int32_t add(const std::string& file, int line, int col,
                    const std::string& function);

        // Look up by id. Caller must pass a valid id (< size()).
        const DbgLoc& at(int32_t id) const;

        size_t size() const { return locs.size(); }
        bool empty() const { return locs.empty(); }
        void clear() { locs.clear(); }

        // All ids whose (file, line) match — the loc_ids a line breakpoint on
        // (file, line) should arm (CP3). `file` matches by exact string.
        std::vector<int32_t> idsForLine(const std::string& file, int line) const;

    private:
        std::vector<DbgLoc> locs;
    };

    // Process-global table backing codegen emission sites for the active
    // debug compile. Single-threaded codegen, so no synchronization.
    DbgLocTable& globalDbgLocTable();

} // namespace cajeta::dbg
