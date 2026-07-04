//
// DAP message framing (CP4). The Debug Adapter Protocol frames each JSON
// message with an HTTP-style header:
//
//   Content-Length: <N>\r\n\r\n<N bytes of UTF-8 JSON>
//
// readMessage/writeMessage work over std::istream/std::ostream so the server
// runs over stdio in production and over stringstreams in tests. Kept separate
// from the dispatcher so the framing is unit-testable on its own.
//
#pragma once

#include <istream>
#include <ostream>

#include "cajeta/dap/Json.h"

namespace cajeta::dap {

    // Serialize `msg` with a Content-Length header and write it to `out`
    // (flushed). Returns false if the stream is bad.
    bool writeMessage(std::ostream& out, const Json& msg);

    // Read one framed message from `in` into *out. Returns true on success;
    // false on EOF / malformed header / a body that fails to parse. Tolerates
    // and skips unknown headers (only Content-Length is required).
    bool readMessage(std::istream& in, Json* out);

} // namespace cajeta::dap
