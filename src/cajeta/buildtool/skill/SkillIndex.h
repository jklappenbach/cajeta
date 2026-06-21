//
// Per-package skill index (skill-discovery spec §2.3, §3). Built from a set of
// SkillDocuments at package-build time, serialized to `skills/index.json` inside
// the `.cja`, and read back to answer canonical-name queries and to generate
// fuzzy candidates (trigram prefilter over names AND titles, feeding the
// matcher in D.4a).
//
#pragma once

#include <map>
#include <string>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>

#include "cajeta/buildtool/skill/SkillDocument.h"

namespace cajeta::buildtool::skill {

    // Where a fuzzy candidate key came from — a canonical name or a skill title.
    enum class MatchSource { Name, Title };

    // One trigram-prefilter candidate: the matched key text, its source, and the
    // skill ids it resolves to (a name may bind several skills; a title binds the
    // one skill that declares it).
    struct SkillCandidate {
        std::string key;
        MatchSource source;
        std::vector<std::string> ids;
    };

    // Per-skill index entry: enough to list it and locate its archive member.
    struct SkillEntry {
        std::string title;  // may be empty
        std::string member; // in-archive path, "skills/<id>.md"
    };

    class SkillIndex {
    public:
        // The index.json schema version (see docs/specs/schema-versioning.md).
        static constexpr llvm::StringLiteral kSchemaVersion = "skill-index-v1";

        // Build an index from validated skill documents. Duplicate skill ids are
        // an error.
        static llvm::Expected<SkillIndex> build(llvm::ArrayRef<SkillDocument> docs);

        // Serialize to deterministic `skills/index.json` text.
        std::string serialize() const;

        // Parse `skills/index.json` text. Rejects an unknown schema version.
        static llvm::Expected<SkillIndex>
        deserialize(llvm::StringRef json, llvm::StringRef sourceName = "skills/index.json");

        // Skill ids bound to `name`. When `hierarchical`, also include skills
        // bound to descendants of `name` (package → its classes/methods, spec
        // §3.2). Result is deterministic (sorted, deduped).
        std::vector<std::string> query(llvm::StringRef name, bool hierarchical) const;

        // Trigram-prefilter candidate keys (names + titles) sharing at least one
        // trigram with `text`. The fuzzy matcher (D.4a) scores these; this is the
        // sub-linear candidate-generation step.
        std::vector<SkillCandidate> candidates(llvm::StringRef text) const;

        // The entry for `id`, or nullptr.
        const SkillEntry* entry(llvm::StringRef id) const;

        // Canonical names bound to `id` (sorted). Lets Search report the matched
        // canonical name for a title hit (spec §3.5).
        std::vector<std::string> namesForId(llvm::StringRef id) const;

        // All skill ids, sorted. Used by List to enumerate (spec §3.6).
        std::vector<std::string> allIds() const;

        size_t skillCount() const { return skills_.size(); }

    private:
        // id → entry; name → ids. std::map for deterministic serialization.
        std::map<std::string, SkillEntry> skills_;
        std::map<std::string, std::vector<std::string>> names_;

        // Searchable keys (names + titles) and a trigram → key-index inverted
        // list. Derived state: rebuilt by build()/deserialize(), not serialized.
        std::vector<SkillCandidate> keys_;
        std::map<std::string, std::vector<size_t>> trigrams_;

        void buildSearchStructures();
    };

} // namespace cajeta::buildtool::skill
