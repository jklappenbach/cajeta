#include "cajeta/dap/DapProtocol.h"

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace cajeta::dap {

bool writeMessage(std::ostream& out, const Json& msg) {
    std::string body = msg.dump();
    out << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    out.flush();
    return static_cast<bool>(out);
}

namespace {

// Read a single header line terminated by \r\n (the \r\n is consumed, not
// returned). Returns false on EOF before a complete line.
bool readHeaderLine(std::istream& in, std::string& line) {
    line.clear();
    int c;
    while ((c = in.get()) != EOF) {
        if (c == '\r') {
            int n = in.get();
            if (n == '\n') return true;
            // Lone \r — treat as part of the line (be lenient).
            line += '\r';
            if (n == EOF) return false;
            line += static_cast<char>(n);
        } else if (c == '\n') {
            return true;  // lenient: accept bare \n too
        } else {
            line += static_cast<char>(c);
        }
    }
    return false;
}

} // namespace

bool readMessage(std::istream& in, Json* out) {
    // Parse headers until a blank line; capture Content-Length.
    long contentLength = -1;
    std::string line;
    while (true) {
        if (!readHeaderLine(in, line)) return false;
        if (line.empty()) break;  // blank line ends the header block
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;  // skip malformed header
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        // trim leading spaces in value
        size_t vs = val.find_first_not_of(" \t");
        if (vs != std::string::npos) val = val.substr(vs);
        // case-insensitive compare for "Content-Length"
        std::string lkey;
        for (char ch : key) lkey += static_cast<char>(std::tolower(ch));
        if (lkey == "content-length") {
            contentLength = std::strtol(val.c_str(), nullptr, 10);
        }
    }
    if (contentLength < 0) return false;

    std::string body(static_cast<size_t>(contentLength), '\0');
    in.read(&body[0], contentLength);
    if (in.gcount() != contentLength) return false;

    bool ok = false;
    Json parsed = Json::parse(body, &ok);
    if (!ok) return false;
    if (out) *out = std::move(parsed);
    return true;
}

} // namespace cajeta::dap
