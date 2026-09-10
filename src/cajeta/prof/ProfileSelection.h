// Which code gets an instrumentation probe. The selection acts at EMISSION,
// never at runtime: excluded code carries no probe at all, and the runtime
// collects every probe that exists without filtering any.
#pragma once

#include <string>
#include <vector>

namespace cajeta::prof {

    // An include/exclude set over canonical class names, one `include`/`exclude`
    // directive per `--profiler-select` line (bare pattern means include, `#`
    // comments). `**` crosses package boundaries in a pattern; `*` does not.
    class ProfileSelection {
    public:
        // Parse selection text; an unparseable line goes to `errors` and is skipped.
        static ProfileSelection parse(const std::string& text,
                                      std::vector<std::string>* errors = nullptr);

        // No directives at all, which selects every class.
        bool empty() const { return inc.empty() && exc.empty(); }

        // The include set defines the universe (empty means everything) and the
        // exclude set subtracts; neither line order nor glob specificity counts.
        bool selects(const std::string& canonicalClassName) const;

        const std::vector<std::string>& includes() const { return inc; }
        const std::vector<std::string>& excludes() const { return exc; }

        // The canonical (sorted, deduped) one-line form recorded in the trace.
        std::string describe() const;

        // Does `pattern` match `name` under the `*` / `**` rules above?
        static bool matches(const std::string& pattern, const std::string& name);

    private:
        std::vector<std::string> inc;
        std::vector<std::string> exc;
    };

} // namespace cajeta::prof
