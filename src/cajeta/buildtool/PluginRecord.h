#pragma once

#include <optional>
#include <string>
#include <vector>

#include "llvm/Support/JSON.h"

namespace cajeta::buildtool {

    // Validation of one plugin protocol record (plugin-output-protocol-spec
    // §4, §5).
    //
    // ONE definition of valid. `PluginRuntime` dispatches on the same rules the
    // conformance suite asserts, so "conforms to spec" cannot mean two
    // different things depending on who is asking (plan §0.2.1). A second
    // validator living in the tests would drift from the one that runs, and the
    // drift would show up as a plugin that passes conformance and fails in a
    // build.
    //
    // The records the protocol defines, and what each REQUIRES beyond `kind`:
    //
    //   log       message                 (level optional; info when absent)
    //   warn      message
    //   write     text
    //   output    key, value
    //   finding   severity, message       (rule/file/line/column optional)
    //   result    status                  (message optional)
    //   error     message
    //
    // Unknown kinds are NOT invalid: a newer plugin emitting a kind this build
    // does not know must not fail against it (§4 use case 2 treats them as
    // unrecognised and drops them with a warning, which is a different thing
    // from malformed).

    enum class RecordVerdict {
        Valid,
        Malformed,   // a known kind missing a required field, or a bad type
        UnknownKind, // well-formed, but this build does not know the kind
    };

    struct RecordCheck {
        RecordVerdict verdict = RecordVerdict::Valid;
        // Why, in a form fit to put in a warning. Empty when Valid.
        std::string reason;

        bool ok() const { return verdict == RecordVerdict::Valid; }
    };

    // Validate a parsed record object.
    RecordCheck checkPluginRecord(const llvm::json::Object& record);

    // Render an untrusted line for inclusion in a diagnostic (§4.1).
    //
    // The line is bytes a plugin controls and is malformed by definition.
    // Quoting it must not let it damage the stream reporting it: control
    // characters and newlines are escaped rather than reproduced (so it cannot
    // forge a second record or console line), the result is bounded (so a
    // plugin cannot flood the log), and invalid UTF-8 is replaced rather than
    // passed through.
    std::string quoteUntrustedLine(llvm::StringRef line, size_t limit = 200);

    // ---- the conformance suite (plan §0) -----------------------------------
    //
    // Whether a PLUGIN conforms, as distinct from whether the runtime copes
    // with one that does not (§3). A plugin author runs this against their own
    // output and gets a yes or a list of reasons; the in-tree plugins run it in
    // CI so the protocol cannot be regressed silently by a later plugin.

    struct ConformanceReport {
        bool passed = true;
        // One entry per problem, each naming the offending line safely.
        std::vector<std::string> problems;
    };

    // Check every line a plugin emitted.
    //
    // Conformance is stricter than the runtime's tolerance, deliberately: the
    // runtime accepts raw text as a log so `printf` debugging keeps working
    // (§4 use case 4), while a plugin that SHIPS raw text has not conformed.
    // Being lenient at runtime and strict here is what lets the protocol hold
    // without breaking anyone mid-build.
    //
    // An UNKNOWN kind passes: emitting a record a given build tool does not
    // know is forward compatibility, not non-conformance.
    ConformanceReport checkPluginStream(llvm::ArrayRef<std::string> lines);

} // namespace cajeta::buildtool
