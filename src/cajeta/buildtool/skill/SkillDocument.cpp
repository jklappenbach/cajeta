//
// Skill document parsing + validation. See SkillDocument.h and
// docs/specs/skill-discovery-spec.md §4.
//
#include "cajeta/buildtool/skill/SkillDocument.h"

#include "cajeta/buildtool/FrontMatter.h"

#include <llvm/ADT/Twine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

namespace cajeta::buildtool::skill {

    namespace {

        llvm::Error fieldError(llvm::StringRef sourceName, const llvm::Twine& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(),
                (sourceName + ": " + msg).str());
        }

        // Map a parsed frontmatter object onto a SkillDocument's fields. Optional
        // fields default to empty; unknown fields (e.g. a stray version) are
        // ignored. Validation happens separately.
        void mapFields(const llvm::json::Object& obj, SkillDocument& out) {
            if (auto id = obj.getString("id")) {
                out.id = id->str();
            }
            if (auto title = obj.getString("title")) {
                out.title = title->str();
            }
            if (auto desc = obj.getString("description")) {
                out.description = desc->str();
            }
            if (const auto* applies = obj.getArray("applies-to")) {
                for (const auto& entry : *applies) {
                    if (auto s = entry.getAsString()) {
                        out.appliesTo.push_back(s->str());
                    } else {
                        // Non-string entry → record an empty marker so validate()
                        // rejects it with a clear message.
                        out.appliesTo.emplace_back();
                    }
                }
            }
        }

        llvm::Expected<SkillDocument>
        fromFrontMatter(FrontMatter fm, llvm::StringRef sourceName) {
            const llvm::json::Object* obj = fm.frontmatter.getAsObject();
            if (!obj) {
                return fieldError(sourceName, "frontmatter is not a YAML mapping");
            }
            if (obj->empty()) {
                return fieldError(
                    sourceName,
                    "missing YAML frontmatter block (expected a '---' header with "
                    "at least 'id' and 'applies-to')");
            }

            SkillDocument doc;
            mapFields(*obj, doc);
            doc.body = std::move(fm.body);
            if (auto e = doc.validate(sourceName)) {
                return std::move(e);
            }
            return doc;
        }

    } // namespace

    llvm::Error SkillDocument::validate(llvm::StringRef sourceName) const {
        if (id.empty()) {
            return fieldError(sourceName, "missing required field 'id'");
        }
        if (appliesTo.empty()) {
            return fieldError(
                sourceName, "missing required field 'applies-to' (expected a "
                            "non-empty list of canonical names)");
        }
        for (const std::string& name : appliesTo) {
            if (name.empty()) {
                return fieldError(
                    sourceName,
                    "field 'applies-to' has an empty or non-string entry");
            }
        }
        return llvm::Error::success();
    }

    llvm::Expected<SkillDocument>
    SkillDocument::parse(std::string_view source, llvm::StringRef sourceName) {
        auto fm = parseFrontMatter(source);
        if (!fm) {
            return fm.takeError();
        }
        return fromFrontMatter(std::move(*fm), sourceName);
    }

    llvm::Expected<SkillDocument> SkillDocument::parseFile(llvm::StringRef path) {
        auto fm = parseFrontMatterFile(path);
        if (!fm) {
            return fm.takeError();
        }
        return fromFrontMatter(std::move(*fm), path);
    }

} // namespace cajeta::buildtool::skill
