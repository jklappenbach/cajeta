//
// cajeta-profiler §3.8-§3.12 — which code gets an instrumentation probe.
//
// The selection acts at EMISSION, never at runtime. Excluded code carries no
// probe to skip and the runtime collects every probe that exists without
// filtering any. That is the whole point: emitting everywhere and discarding at
// runtime pays the full price to show less, so a narrow selection would become
// a display preference instead of an overhead reduction.
//
#pragma once

#include <string>
#include <vector>

namespace cajeta::prof {

    // An include/exclude set over canonical class names, parsed from the text
    // of a `--profiler-select` file.
    //
    // Syntax, one directive per line; `#` to end-of-line is a comment:
    //
    //     include dev.cajeta.engine.**     # the package and everything under it
    //     include dev.cajeta.Bar           # one class
    //     exclude dev.cajeta.engine.Debug*
    //
    // A bare pattern with no keyword is an `include` (the common case reads
    // better without the noise). In a pattern, `**` matches any run of
    // characters INCLUDING `.` (so it crosses package boundaries) and `*`
    // matches any run within one segment (so it does not).
    class ProfileSelection {
    public:
        // Parse selection text. Unparseable lines are appended to `errors`
        // (when given) and skipped, so one typo does not silently widen the
        // selection to everything.
        static ProfileSelection parse(const std::string& text,
                                      std::vector<std::string>* errors = nullptr);

        // No directives at all — every class is selected. This is what a build
        // with `--profiler=instrument` and no `--profiler-select` gets.
        bool empty() const { return inc.empty() && exc.empty(); }

        // §3.9, the ONE rule: the include set defines the universe (an empty
        // include set means "everything") and the exclude set subtracts from
        // it. Never ordering, never pattern specificity — a class's membership
        // must not depend on which line came first or which glob is tighter.
        bool selects(const std::string& canonicalClassName) const;

        const std::vector<std::string>& includes() const { return inc; }
        const std::vector<std::string>& excludes() const { return exc; }

        // §3.12 — the one-line form recorded in the trace, so a profile that
        // omits code says so instead of reading as though that code were free.
        // Canonical (sorted, deduped, comments dropped) because it is a
        // description of the selection in force, not an echo of the file.
        std::string describe() const;

        // Does `pattern` match `name` under the `*` / `**` rules above?
        // Exposed for the parser's own use and for tests.
        static bool matches(const std::string& pattern, const std::string& name);

    private:
        std::vector<std::string> inc;
        std::vector<std::string> exc;
    };

} // namespace cajeta::prof
