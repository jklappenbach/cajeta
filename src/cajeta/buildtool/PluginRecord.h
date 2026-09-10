#pragma once

#include <optional>
#include <string>
#include <vector>

#include "llvm/Support/JSON.h"

namespace cajeta::buildtool {

    // Validation of one plugin protocol record, and the ONE definition of valid: the
    // runtime and the conformance suite dispatch on these same rules. An unknown kind
    // is NOT invalid — a newer build's record is dropped with a warning.

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

    // Render an untrusted line for a diagnostic. The bytes are plugin-controlled and
    // malformed by definition, so control characters and newlines are escaped rather
    // than reproduced, the result is bounded, and invalid UTF-8 is replaced.
    std::string quoteUntrustedLine(llvm::StringRef line, size_t limit = 200);

    // ---- the conformance suite ---------------------------------------------

    struct ConformanceReport {
        bool passed = true;
        // One entry per problem, each naming the offending line safely.
        std::vector<std::string> problems;
    };

    // Check every line a plugin emitted. Deliberately stricter than the runtime, which
    // accepts raw text as a log so `printf` debugging keeps working while a plugin that
    // SHIPS raw text has not conformed. An UNKNOWN kind passes: that is compatibility.
    ConformanceReport checkPluginStream(llvm::ArrayRef<std::string> lines);

} // namespace cajeta::buildtool
