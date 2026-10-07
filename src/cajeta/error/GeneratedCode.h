// Generated-code locations (specs/diagnostic-location-spec.md 3): which generator produced the code
// being compiled, the declaration that asked for it, and where its inner positions map back to.

#pragma once

#include <string>

#include "Exception.h"

namespace cajeta {

    // The compiler's own generator names; `via` is free text, so a new generator needs no schema change.
    inline constexpr const char* kViaTemplate = "template";
    inline constexpr const char* kViaDefaultConstructor = "default-constructor";

    // A body synthesizer's name: its registry label in kebab case, then `-synthesizer`.
    std::string synthesizerVia(const std::string& label);

    // A source position; a non-positive line is unknown.
    struct SourceSite {
        std::string file;
        int line = -1;
        int column = -1;
        bool known() const { return line > 0; }
    };

    // What generated the code being compiled; an empty `via` is source. `request` is where the user
    // is pointed, the bound record for a codec (`bindsRecord`); an inner position becomes `from`
    // after `lineDelta`, or `anchor` when inner positions are not source.
    struct GenerationSite {
        std::string via;
        SourceSite request;
        SourceSite anchor;
        bool anchorOnly = false;
        bool bindsRecord = false;
        int lineDelta = 0;
        std::string innerFile;
    };

    // Pushes `site` for this thread while in scope. A source site masks any outer generated one.
    class GenerationScope {
    public:
        explicit GenerationScope(const GenerationSite& site);
        ~GenerationScope();
        GenerationScope(const GenerationScope&) = delete;
        GenerationScope& operator=(const GenerationScope&) = delete;
    };

    // The innermost site when it is generated code, else null.
    const GenerationSite* currentGenerationSite();

    // Records the source position now being compiled, so an instantiation can name its requester.
    class RequestCursor {
    public:
        RequestCursor(const std::string& file, int line, int column);
        ~RequestCursor();
        RequestCursor(const RequestCursor&) = delete;
        RequestCursor& operator=(const RequestCursor&) = delete;
    };

    // The declaration asking for code now: an enclosing generated site's request, else the cursor.
    SourceSite currentRequestSite();

    // Moves (file, line, column) to `site`'s request and returns the origin the move records.
    GeneratedOrigin applyGenerationSite(const GenerationSite& site,
                                        std::string& file, int& line, int& column);

    // The text diagnostic's trailing clause for generated code, empty for source.
    std::string generatedClause(const GeneratedOrigin& origin);

} // namespace cajeta
