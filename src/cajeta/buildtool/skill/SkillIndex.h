// Per-package skill index: built from SkillDocuments at package-build time and
// serialized to `skills/index.json` in the `.cja`, for name and fuzzy queries.
#pragma once

#include <map>
#include <string>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>

#include "cajeta/buildtool/skill/SkillDocument.h"

namespace cajeta::buildtool::skill {

    enum class MatchSource { Name, Title };   // where a candidate key came from

    // A candidate key: a name may bind several skills, a title binds only one.
    struct SkillCandidate {
        std::string key;
        MatchSource source;
        std::vector<std::string> ids;
    };

    struct SkillEntry {
        std::string title;  // may be empty
        std::string member; // in-archive path, "skills/<id>.md"
    };

    class SkillIndex {
    public:
        static constexpr llvm::StringLiteral kSchemaVersion = "skill-index-v1";

        // Build from validated documents; duplicate skill ids are an error.
        static llvm::Expected<SkillIndex> build(llvm::ArrayRef<SkillDocument> docs);

        std::string serialize() const;   // deterministic `skills/index.json` text

        // Parse `skills/index.json` text. Rejects an unknown schema version.
        static llvm::Expected<SkillIndex>
        deserialize(llvm::StringRef json, llvm::StringRef sourceName = "skills/index.json");

        // Skill ids bound to `name`, sorted and deduped. When `hierarchical`,
        // also include skills bound to descendants of `name`.
        std::vector<std::string> query(llvm::StringRef name, bool hierarchical) const;

        // Keys sharing a trigram with `text` — the prefilter the matcher scores.
        std::vector<SkillCandidate> candidates(llvm::StringRef text) const;

        const SkillEntry* entry(llvm::StringRef id) const;   // nullptr if unknown

        // Canonical names bound to `id`, sorted — what Search reports for a title hit.
        std::vector<std::string> namesForId(llvm::StringRef id) const;

        std::vector<std::string> allIds() const;   // every skill id, sorted

        size_t skillCount() const { return skills_.size(); }

    private:
        // std::map throughout, for deterministic serialization.
        std::map<std::string, SkillEntry> skills_;
        std::map<std::string, std::vector<std::string>> names_;

        std::vector<SkillCandidate> keys_;   // derived; rebuilt, never serialized
        std::map<std::string, std::vector<size_t>> trigrams_;

        void buildSearchStructures();
    };

} // namespace cajeta::buildtool::skill
