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
        // After setAt() replay, appends continue past the max replayed id, so
        // fresh codegen can never collide with a cached module's baked ids.
        int32_t add(const std::string& file, int line, int col,
                    const std::string& function);

        // Sparse replay (fast-debug-launch 3.2.1): place `loc` at exactly
        // `id`, growing the table with HOLES as needed. A hole is a default
        // DbgLoc (empty file, line 0); at() returns it harmlessly and
        // idsForLine() never matches one. Used to restore a cached module's
        // loc ids, which are baked into its safepoint calls as constants.
        void setAt(int32_t id, DbgLoc loc);

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

    // Loc-table sidecar (fast-debug-launch 3.2.2) — the persistence pair for
    // cache slots. The format is sparse-native (one line per NON-hole entry,
    // strings escaped for tab/newline/backslash), so holes round-trip for
    // free. write returns false on I/O failure; load returns false on a
    // missing/malformed file and leaves `into` in an unspecified partial
    // state — callers treat false as "no sidecar, fall back to compiling".
    bool writeDbgLocSidecar(const std::string& path, const DbgLocTable& table);
    bool loadDbgLocSidecar(const std::string& path, DbgLocTable& into);

} // namespace cajeta::dbg
