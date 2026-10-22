// Build-time skill packaging. See SkillPackager.h.
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

        // Sorted so packaging never depends on directory iteration order.
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

    namespace {
        bool hasSkillFiles(const std::filesystem::path& dir) {
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) return false;
            for (std::filesystem::directory_iterator it(dir, ec), end;
                 !ec && it != end; it.increment(ec)) {
                if (it->is_regular_file(ec) && it->path().extension() == ".md")
                    return true;
            }
            return false;
        }

        std::filesystem::path normal(llvm::StringRef p) {
            std::error_code ec;
            auto a = std::filesystem::absolute(std::filesystem::path(p.str()), ec);
            return std::filesystem::weakly_canonical(ec ? std::filesystem::path(p.str()) : a, ec);
        }
    } // namespace

    std::vector<std::string> misplacedSkillWarnings(llvm::StringRef skillRoot,
                                                    llvm::StringRef sourceRoot) {
        namespace fs = std::filesystem;
        std::vector<std::string> out;
        fs::path skills = normal(skillRoot) / "skills";
        fs::path src = normal(sourceRoot);
        std::error_code ec;

        // A skills/ directory inside the source tree, other than the one packed.
        std::vector<fs::path> nested;
        if (fs::is_directory(src, ec)) {
            for (fs::recursive_directory_iterator it(src, ec), end;
                 !ec && it != end; it.increment(ec)) {
                if (!it->is_directory(ec) || it->path().filename() != "skills") continue;
                if (normal(it->path().string()) == skills) continue;
                if (hasSkillFiles(it->path())) nested.push_back(it->path());
            }
        }
        std::sort(nested.begin(), nested.end());
        for (const auto& d : nested) {
            out.push_back("skills in " + d.generic_string() +
                          " are not packed: the packager reads only " +
                          skills.generic_string() +
                          "/*.md, so move them to the project's skills/");
        }

        // The project's own skills/, beside the nearest cajeta.json at or
        // above the source root, when the skill root is elsewhere.
        for (fs::path cur = src; !cur.empty(); cur = cur.parent_path()) {
            if (fs::exists(cur / "cajeta.json", ec)) {
                fs::path project = cur / "skills";
                if (normal(project.string()) != skills && hasSkillFiles(project)) {
                    out.push_back(project.generic_string() +
                                  " is not packed: the skill root is " +
                                  normal(skillRoot).generic_string() +
                                  "; pass --skill-root=" + cur.generic_string() +
                                  " (cajeta build passes the project root)");
                }
                break;
            }
            if (cur == cur.parent_path()) break;
        }
        return out;
    }

} // namespace cajeta::buildtool::skill
