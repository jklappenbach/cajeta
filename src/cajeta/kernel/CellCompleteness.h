//
// jupyter-kernel U5 (spec 3.4) — the `is_complete_request` triage.
//
// A frontend asks this on every Enter in a console prompt: submit the cell,
// or open another line? The answer is not "does it compile" — a cell that
// references an undeclared name is COMPLETE and should be submitted so the
// user sees the error. The question is purely syntactic: did the text run out
// before the grammar did?
//
// So the classifier parses and looks at WHERE the first syntax error is. An
// error whose offending token is EOF means the parser wanted more input:
// incomplete. An error anywhere else is a genuine mistake the user will not
// fix by typing another line: invalid. No errors: complete.
//
#pragma once

#include <string>

namespace cajeta::kernel {

    enum class Completeness { Complete, Incomplete, Invalid };

    const char* completenessName(Completeness c);

    // `indent` (non-null) receives the continuation indent a frontend should
    // pre-fill on an INCOMPLETE verdict — four spaces per unclosed brace, and
    // empty for every other verdict.
    Completeness classifyCell(const std::string& source,
                              std::string* indent = nullptr);

}  // namespace cajeta::kernel
