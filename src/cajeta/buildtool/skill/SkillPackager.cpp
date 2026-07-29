//
// Build-time skill packaging. See SkillPackager.h and
// specs/archive/skill-discovery-spec.md §4.2.
//
#include "cajeta/buildtool/skill/SkillPackager.h"

#include "cajeta/buildtool/skill/SkillDocument.h"
#include "cajeta/buildtool/skill/SkillIndex.h"
#include "cajeta/compile/CajetaArchive.h"

#include <llvm/ADT/Twine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>

#include <algorithm>
#include <filesystem>
#include <stdexcept>

namespace cajeta::buildtool::skill {

    namespace {

        llvm::Error packageError(const llvm::Twine& msg) {
            return llvm::createStringError(llvm::inconvertibleErrorCode(), msg.str());
        }

    } // namespace

    llvm::Expected<std::vector<SkillMember>>
    buildSkillMembers(llvm::StringRef packageRoot) {
        namespace fs = std::filesystem;
        fs::path skillsDir = fs::path(packageRoot.str()) / "skills";

        std::error_code ec;
        if (!fs::is_directory(skillsDir, ec)) {
            return std::vector<SkillMember>{}; // no skills — not an error
        }

        // Collect *.md sources, sorted for deterministic processing.
        std::vector<fs::path> sources;
        for (fs::directory_iterator it(skillsDir, ec), end; !ec && it != end;
             it.increment(ec)) {
            const fs::path& p = it->path();
            if (it->is_regular_file(ec) && p.extension() == ".md") {
                sources.push_back(p);
            }
        }
        std::sort(sources.begin(), sources.end());

        std::vector<SkillDocument> docs;
        std::vector<std::string> rawBytes; // parallel to docs
        for (const fs::path& p : sources) {
            auto buf = llvm::MemoryBuffer::getFile(p.string());
            if (!buf) {
                return packageError("cannot read skill '" + p.string() +
                                    "': " + buf.getError().message());
            }
            std::string raw = (*buf)->getBuffer().str();
            auto doc = SkillDocument::parse(raw, p.string());
            if (!doc) {
                return doc.takeError();
            }
            docs.push_back(std::move(*doc));
            rawBytes.push_back(std::move(raw));
        }

        if (docs.empty()) {
            return std::vector<SkillMember>{};
        }

        auto index = SkillIndex::build(docs);
        if (!index) {
            return index.takeError();
        }

        std::vector<SkillMember> members;
        members.reserve(docs.size() + 1);
        for (size_t i = 0; i < docs.size(); ++i) {
            members.push_back({"skills/" + docs[i].id + ".md", std::move(rawBytes[i])});
        }
        members.push_back({"skills/index.json", index->serialize()});

        // Reproducible: stable order by in-archive path.
        std::sort(members.begin(), members.end(),
                  [](const SkillMember& a, const SkillMember& b) {
                      return a.path < b.path;
                  });
        return members;
    }

    llvm::Error addSkillMembersToArchive(CajetaArchive& archive,
                                         llvm::StringRef packageRoot) {
        auto members = buildSkillMembers(packageRoot);
        if (!members) {
            return members.takeError();
        }
        for (const SkillMember& m : *members) {
            CajetaArchiveEntry entry;
            entry.name = m.path;
            entry.originTag = static_cast<uint8_t>(CajetaArchive::Origin::User);
            entry.kindTag = CajetaArchive::EntryKind::Resource;
            entry.data.assign(m.bytes.begin(), m.bytes.end());
            archive.addEntry(std::move(entry));
        }
        return llvm::Error::success();
    }

    void addSkillMembersToArchiveOrThrow(CajetaArchive& archive,
                                         llvm::StringRef packageRoot) {
        if (auto e = addSkillMembersToArchive(archive, packageRoot)) {
            throw std::runtime_error(llvm::toString(std::move(e)));
        }
    }

} // namespace cajeta::buildtool::skill
