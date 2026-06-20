//
// Front-matter Markdown splitting. See FrontMatter.h and
// docs/specs/yaml-frontmatter-spec.md §2.
//
#include "cajeta/buildtool/FrontMatter.h"

#include <llvm/Support/Error.h>

namespace cajeta::buildtool {

    namespace {

        constexpr std::string_view kBom = "\xEF\xBB\xBF";

        // A single line read out of the source.
        struct Line {
            std::string_view content; // line text, trailing '\r' removed (for fence compares)
            size_t next;              // index just past the line's '\n' (or source size)
        };

        // Read the line beginning at `pos`. The returned `content` has any trailing
        // '\r' stripped so CRLF and LF fences compare equal; `next` always points at
        // the first byte of the following line (or the end of source). These byte
        // scans are isolated so a SIMD newline scan can replace them later
        // (spec §5) without touching the split logic.
        Line readLine(std::string_view s, size_t pos) {
            size_t nl = s.find('\n', pos);
            if (nl == std::string_view::npos) {
                std::string_view content = s.substr(pos);
                if (!content.empty() && content.back() == '\r') {
                    content.remove_suffix(1);
                }
                return {content, s.size()};
            }
            std::string_view content = s.substr(pos, nl - pos);
            if (!content.empty() && content.back() == '\r') {
                content.remove_suffix(1);
            }
            return {content, nl + 1};
        }

        bool isClosingFence(std::string_view content) {
            return content == "---" || content == "...";
        }

    } // namespace

    llvm::Expected<FrontMatterSplit> splitFrontMatter(std::string_view source) {
        FrontMatterSplit out;

        // Detection ignores a leading BOM, but body bytes are preserved as-is.
        size_t start = 0;
        if (source.substr(0, kBom.size()) == kBom) {
            start = kBom.size();
        }

        Line first = readLine(source, start);
        if (first.content != "---") {
            // No frontmatter: the entire input is the body, byte-for-byte.
            out.present = false;
            out.body.assign(source.data(), source.size());
            return out;
        }

        out.present = true;
        size_t pos = first.next;
        size_t headerStart = pos;
        bool closed = false;
        while (pos < source.size()) {
            Line line = readLine(source, pos);
            if (isClosingFence(line.content)) {
                // Header is the raw bytes between the fences (fence excluded).
                out.header.assign(source.data() + headerStart, pos - headerStart);
                pos = line.next;
                closed = true;
                break;
            }
            pos = line.next;
        }

        if (!closed) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(),
                "front matter: opening '---' fence has no closing fence");
        }

        // Body is everything after the closing fence line, byte-for-byte.
        out.body.assign(source.data() + pos, source.size() - pos);
        return out;
    }

} // namespace cajeta::buildtool
