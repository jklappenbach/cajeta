// Wrapper-line → host-line translation for script units (spec §6.1): each spliced
// segment records where it landed and where it came from. ANTLR-free on purpose,
// since CajetaModule stores the map and must not pull in the parser headers.
#pragma once

#include <vector>

namespace cajeta {

    struct ScriptLineSpan {
        int wrapperStart = 0;  // 1-based first wrapper line of the segment
        int hostStart = 0;     // 1-based first host line of the segment
        int count = 0;         // lines in the segment
    };

    using ScriptLineMap = std::vector<ScriptLineSpan>;

    // Translates a 1-based wrapper line to its host line: inside a span by offset,
    // between spans to the nearest preceding span's last host line, and 0 when
    // nothing precedes or the map is empty.
    inline int mapScriptLine(const ScriptLineMap& map, int wrapperLine) {
        if (map.empty() || wrapperLine <= 0) return wrapperLine;
        int best = 0;
        for (const auto& span : map) {
            if (wrapperLine < span.wrapperStart) break;
            int offset = wrapperLine - span.wrapperStart;
            if (offset < span.count) return span.hostStart + offset;
            best = span.hostStart + span.count - 1;
        }
        return best;
    }

}  // namespace cajeta
