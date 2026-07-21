#include "cajeta/dbg/DebugLocTable.h"

#include <cassert>
#include <fstream>
#include <sstream>

namespace cajeta::dbg {

namespace {

    // Sidecar string escaping: the format is line- and tab-delimited, so the
    // three structural characters are escaped; everything else is verbatim.
    std::string escape(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            switch (c) {
                case '\\': out += "\\\\"; break;
                case '\t': out += "\\t"; break;
                case '\n': out += "\\n"; break;
                default: out += c;
            }
        }
        return out;
    }

    bool unescape(const std::string& s, std::string& out) {
        out.clear();
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] != '\\') { out += s[i]; continue; }
            if (++i >= s.size()) return false;  // dangling escape
            switch (s[i]) {
                case '\\': out += '\\'; break;
                case 't': out += '\t'; break;
                case 'n': out += '\n'; break;
                default: return false;
            }
        }
        return true;
    }

} // namespace

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
            if (locs[i].file.empty()) continue;  // sparse-replay hole
            if (locs[i].line == line && locs[i].file == file) {
                out.push_back(static_cast<int32_t>(i));
            }
        }
        return out;
    }

    void DbgLocTable::setAt(int32_t id, DbgLoc loc) {
        assert(id >= 0 && "DbgLocTable::setAt negative id");
        size_t idx = static_cast<size_t>(id);
        if (idx >= locs.size()) locs.resize(idx + 1);  // holes are default DbgLoc
        locs[idx] = std::move(loc);
    }

    DbgLocTable& globalDbgLocTable() {
        static DbgLocTable table;
        return table;
    }

    bool writeDbgLocSidecar(const std::string& path, const DbgLocTable& table) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << "cajeta-dbgloc-v1\n";
        for (size_t id = 0; id < table.size(); ++id) {
            const DbgLoc& loc = table.at(static_cast<int32_t>(id));
            if (loc.file.empty() && loc.line == 0) continue;  // hole
            out << id << '\t' << loc.line << '\t' << loc.col << '\t'
                << escape(loc.file) << '\t' << escape(loc.function) << '\n';
        }
        return out.good();
    }

    bool loadDbgLocSidecar(const std::string& path, DbgLocTable& into) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::string line;
        if (!std::getline(in, line) || line != "cajeta-dbgloc-v1") return false;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            std::istringstream fields(line);
            std::string idStr, lineStr, colStr, fileEsc, fnEsc;
            if (!std::getline(fields, idStr, '\t') ||
                !std::getline(fields, lineStr, '\t') ||
                !std::getline(fields, colStr, '\t') ||
                !std::getline(fields, fileEsc, '\t') ||
                !std::getline(fields, fnEsc)) return false;
            DbgLoc loc;
            int32_t id;
            try {
                id = static_cast<int32_t>(std::stol(idStr));
                loc.line = std::stoi(lineStr);
                loc.col = std::stoi(colStr);
            } catch (...) {
                return false;
            }
            if (id < 0) return false;
            if (!unescape(fileEsc, loc.file) ||
                !unescape(fnEsc, loc.function)) return false;
            into.setAt(id, std::move(loc));
        }
        return true;
    }

} // namespace cajeta::dbg
