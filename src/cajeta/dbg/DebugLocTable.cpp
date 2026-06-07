#include "cajeta/dbg/DebugLocTable.h"

#include <cassert>

namespace cajeta::dbg {

    int32_t DbgLocTable::add(const std::string& file, int line, int col,
                             const std::string& function) {
        int32_t id = static_cast<int32_t>(locs.size());
        locs.push_back(DbgLoc{file, line, col, function});
        return id;
    }

    const DbgLoc& DbgLocTable::at(int32_t id) const {
        assert(id >= 0 && static_cast<size_t>(id) < locs.size()
               && "DbgLocTable::at id out of range");
        return locs[static_cast<size_t>(id)];
    }

    std::vector<int32_t> DbgLocTable::idsForLine(const std::string& file,
                                                 int line) const {
        std::vector<int32_t> out;
        for (size_t i = 0; i < locs.size(); ++i) {
            if (locs[i].line == line && locs[i].file == file) {
                out.push_back(static_cast<int32_t>(i));
            }
        }
        return out;
    }

    DbgLocTable& globalDbgLocTable() {
        static DbgLocTable table;
        return table;
    }

} // namespace cajeta::dbg
