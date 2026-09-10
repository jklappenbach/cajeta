// Front-matter Markdown splitting. See FrontMatter.h and specs/archive/yaml-frontmatter-spec.md §2.
#include "cajeta/buildtool/FrontMatter.h"

#include "cajeta/buildtool/Yaml.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>

namespace cajeta::buildtool {

    namespace {

        constexpr std::string_view kBom = "\xEF\xBB\xBF";

        struct Line {
            std::string_view content; // line text, trailing '\r' removed (for fence compares)
            size_t next;              // index just past the line's '\n' (or source size)
        };

        // Reads the line at `pos`; `content` has any trailing '\r' stripped so CRLF
        // and LF fences compare equal, and `next` points at the following line.
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

        out.body.assign(source.data() + pos, source.size() - pos);
        return out;
    }

    llvm::Expected<FrontMatter> parseFrontMatter(std::string_view source) {
        auto split = splitFrontMatter(source);
        if (!split) {
            return split.takeError();
        }

        FrontMatter out;
        out.body = std::move(split->body);
        if (!split->present) {
            return out;
        }

        // The header's first line is the document line after the opening `---`, so
        // YAML errors report document-absolute lines.
        auto value = parseYaml(split->header, 2);
        if (!value) {
            return value.takeError();
        }
        out.frontmatter = std::move(*value);
        return out;
    }

    llvm::Expected<FrontMatter> parseFrontMatterFile(llvm::StringRef path) {
        auto buffer = llvm::MemoryBuffer::getFile(path);
        if (!buffer) {
            return llvm::createStringError(
                buffer.getError(),
                ("front matter: cannot read '" + path + "'").str());
        }

        auto parsed = parseFrontMatter((*buffer)->getBuffer());
        if (!parsed) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(),
                (path + ": " + llvm::toString(parsed.takeError())).str());
        }
        return parsed;
    }

} // namespace cajeta::buildtool
