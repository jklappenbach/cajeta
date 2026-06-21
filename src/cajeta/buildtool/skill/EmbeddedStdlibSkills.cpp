// See EmbeddedStdlibSkills.h. Decompresses the embedded stdlib skill corpus
// (tools/skillembed framing), groups by library, parses each member, and builds
// one ResolvedSkillArchive per library. Built once and cached.

#include "cajeta/buildtool/skill/EmbeddedStdlibSkills.h"

#include "cajeta/buildtool/skill/SkillDocument.h"
#include "cajeta/buildtool/skill/SkillIndex.h"
#include "cajeta/compile/StdlibSkillsEmbedded.h"

#include <zstd.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <llvm/Support/Error.h>

namespace cajeta::buildtool::skill {

    const char* const kStdlibSkillVersion = "1.0";

    namespace {

        struct Embedded {
            std::vector<ResolvedSkillArchive> archives;
            // key = library + '\x1f' + id  ->  decompressed payload
            std::map<std::string, std::string> payloads;
        };

        const Embedded& load() {
            static const Embedded e = [] {
                Embedded out;

                const size_t rawLen = stdlib::g_stdlibSkillsUncompressedLen;
                if (rawLen == 0 || stdlib::g_stdlibSkillsCompressedLen == 0) return out;

                std::string blob;
                blob.resize(rawLen);
                size_t n = ZSTD_decompress(blob.data(), blob.size(),
                                           stdlib::g_stdlibSkillsCompressed,
                                           stdlib::g_stdlibSkillsCompressedLen);
                if (ZSTD_isError(n) || n != blob.size()) return out;   // corrupt → empty

                size_t off = 0;
                auto rdU32 = [&](uint32_t& v) -> bool {
                    if (off + 4 > blob.size()) return false;
                    v = (uint8_t) blob[off] | ((uint8_t) blob[off + 1] << 8)
                      | ((uint8_t) blob[off + 2] << 16) | ((uint8_t) blob[off + 3] << 24);
                    off += 4;
                    return true;
                };

                uint32_t count = 0;
                if (!rdU32(count)) return out;

                std::map<std::string, std::vector<SkillDocument>> byLib;
                for (uint32_t i = 0; i < count; ++i) {
                    uint32_t libLen = 0, contentLen = 0;
                    if (!rdU32(libLen) || off + libLen > blob.size()) break;
                    std::string lib = blob.substr(off, libLen);
                    off += libLen;
                    if (!rdU32(contentLen) || off + contentLen > blob.size()) break;
                    std::string content = blob.substr(off, contentLen);
                    off += contentLen;

                    auto doc = SkillDocument::parse(content, lib);
                    if (!doc) { consumeError(doc.takeError()); continue; }
                    out.payloads[lib + '\x1f' + doc->id] = content;
                    byLib[lib].push_back(std::move(*doc));
                }

                for (auto& kv : byLib) {
                    auto idx = SkillIndex::build(kv.second);
                    if (!idx) { consumeError(idx.takeError()); continue; }
                    out.archives.push_back(
                        ResolvedSkillArchive{kv.first, kStdlibSkillVersion, std::move(*idx)});
                }
                return out;
            }();
            return e;
        }

    } // namespace

    const std::vector<ResolvedSkillArchive>& embeddedStdlibSkillArchives() {
        return load().archives;
    }

    std::optional<std::string> embeddedStdlibSkillPayload(
        llvm::StringRef library, llvm::StringRef id) {
        const auto& m = load().payloads;
        auto it = m.find((library + "\x1f" + id).str());
        if (it == m.end()) return std::nullopt;
        return it->second;
    }

} // namespace cajeta::buildtool::skill
