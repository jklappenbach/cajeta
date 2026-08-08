//
// script-units U5 (spec §6.1) — wrapper-line → host-line translation.
//
// The script synthesis splices original member text into the implicit-class
// wrapper, shifting line numbers. Each spliced segment records where it
// landed (wrapperStart) and where it came from (hostStart); internal
// newlines are preserved verbatim, so a line inside a segment maps by
// offset. Synthetic wrapper lines (package default, class shell, appended
// `return 0;`) fall between segments and resolve to the nearest PRECEDING
// host line — the closest thing the user wrote.
//
// This header is ANTLR-free on purpose: CajetaModule stores the map, and
// CajetaModule.h must not pull in the parser headers.
//
#pragma once

#include <vector>

namespace cajeta {

    struct ScriptLineSpan {
        int wrapperStart = 0;  // 1-based first wrapper line of the segment
        int hostStart = 0;     // 1-based first host line of the segment
        int count = 0;         // lines in the segment
    };

    using ScriptLineMap = std::vector<ScriptLineSpan>;

    // Translate a 1-based wrapper line to its host line. Lines inside a
    // span map by offset; lines between spans resolve to the nearest
    // preceding span's last host line; 0 when nothing precedes (an
    // all-synthetic prefix) or the map is empty.
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
