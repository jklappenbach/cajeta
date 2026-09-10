// DAP message framing (CP4): each JSON message carries an HTTP-style `Content-Length:
// <N>` header. Works over any istream/ostream, so the framing is unit-testable alone.
#pragma once

#include <istream>
#include <ostream>

#include "cajeta/dap/Json.h"

namespace cajeta::dap {

    // Serializes `msg` with a Content-Length header and flushes it to `out`; false if the stream is bad.
    bool writeMessage(std::ostream& out, const Json& msg);

    // Reads one framed message from `in` into *out; false on EOF, a malformed header,
    // or a body that fails to parse. Unknown headers are skipped.
    bool readMessage(std::istream& in, Json* out);

} // namespace cajeta::dap
