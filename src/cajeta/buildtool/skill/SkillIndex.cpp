//
// Per-package skill index build / serialize / query / candidate generation.
// See SkillIndex.h and docs/specs/skill-discovery-spec.md §2.3, §3.
//
#include "cajeta/buildtool/skill/SkillIndex.h"

#include <llvm/ADT/Twine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cctype>
#include <set>

namespace cajeta::buildtool::skill {

    namespace {

        llvm::Error indexError(llvm::StringRef sourceName, const llvm::Twine& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), (sourceName + ": " + msg).str());
        }

        // Lowercased trigrams of `text` (whole string when shorter than 3). Used
        // only for candidate prefiltering, so case-folding widens the net.
        std::vector<std::string> trigramsOf(llvm::StringRef text) {
            std::string s;
            s.reserve(text.size());
            for (char c : text) {
                s.push_back(static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c))));
            }
            std::vector<std::string> out;
            if (s.size() < 3) {
                if (!s.empty()) out.push_back(s);
                return out;
            }
            for (size_t i = 0; i + 3 <= s.size(); ++i) {
                out.push_back(s.substr(i, 3));
            }
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
            return out;
        }

        // True iff `key` is a hierarchical descendant of `name`: it extends `name`
        // at a segment boundary ('/' between packages/classes, '.' before a
        // method). E.g. cajeta/torch/nn ⊃ cajeta/torch/nn/Linear ⊃ …/Linear.fwd.
        bool isDescendant(llvm::StringRef key, llvm::StringRef name) {
            if (key.size() <= name.size() || !key.starts_with(name)) {
                return false;
            }
            char sep = key[name.size()];
            return sep == '/' || sep == '.';
        }

        // Emit a JSON string token (quoted + escaped) for deterministic output.
        std::string jstr(llvm::StringRef s) {
            std::string out;
            llvm::raw_string_ostream os(out);
            os << llvm::json::Value(s);
            return os.str();
        }

    } // namespace

    void SkillIndex::buildSearchStructures() {
        keys_.clear();
        trigrams_.clear();
        // One key per canonical name (resolving to its ids)…
        for (const auto& [name, ids] : names_) {
            keys_.push_back({name, MatchSource::Name, ids});
        }
        // …and one per non-empty title (resolving to its single skill id).
        for (const auto& [id, entry] : skills_) {
            if (!entry.title.empty()) {
                keys_.push_back({entry.title, MatchSource::Title, {id}});
            }
        }
        for (size_t i = 0; i < keys_.size(); ++i) {
            for (const std::string& g : trigramsOf(keys_[i].key)) {
                trigrams_[g].push_back(i);
            }
        }
    }

    llvm::Expected<SkillIndex> SkillIndex::build(llvm::ArrayRef<SkillDocument> docs) {
        SkillIndex idx;
        for (const SkillDocument& d : docs) {
            if (idx.skills_.count(d.id)) {
                return indexError("skills/index.json",
                                  "duplicate skill id '" + d.id + "'");
            }
            idx.skills_[d.id] = SkillEntry{d.title, "skills/" + d.id + ".md"};
            for (const std::string& name : d.appliesTo) {
                idx.names_[name].push_back(d.id);
            }
        }
        // Deterministic, deduped id lists per name.
        for (auto& [name, ids] : idx.names_) {
            std::sort(ids.begin(), ids.end());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        }
        idx.buildSearchStructures();
        return idx;
    }

    std::string SkillIndex::serialize() const {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << "{\n";
        os << "  \"schema-version\": " << jstr(kSchemaVersion) << ",\n";

        os << "  \"names\": {";
        bool first = true;
        for (const auto& [name, ids] : names_) {
            os << (first ? "\n" : ",\n") << "    " << jstr(name) << ": [";
            for (size_t i = 0; i < ids.size(); ++i) {
                os << (i ? ", " : "") << jstr(ids[i]);
            }
            os << "]";
            first = false;
        }
        os << (first ? "" : "\n  ") << "},\n";

        os << "  \"skills\": {";
        first = true;
        for (const auto& [id, entry] : skills_) {
            os << (first ? "\n" : ",\n") << "    " << jstr(id) << ": {\"title\": "
               << jstr(entry.title) << ", \"member\": " << jstr(entry.member) << "}";
            first = false;
        }
        os << (first ? "" : "\n  ") << "}\n";

        os << "}\n";
        return os.str();
    }

    llvm::Expected<SkillIndex>
    SkillIndex::deserialize(llvm::StringRef json, llvm::StringRef sourceName) {
        auto parsed = llvm::json::parse(json);
        if (!parsed) {
            return indexError(sourceName, llvm::toString(parsed.takeError()));
        }
        const llvm::json::Object* root = parsed->getAsObject();
        if (!root) {
            return indexError(sourceName, "index is not a JSON object");
        }

        auto schema = root->getString("schema-version");
        if (!schema) {
            return indexError(sourceName, "missing 'schema-version'");
        }
        if (*schema != kSchemaVersion) {
            return indexError(
                sourceName,
                "unsupported schema-version '" + *schema + "' (supported: " +
                    llvm::Twine(kSchemaVersion) + ")");
        }

        SkillIndex idx;
        if (const llvm::json::Object* skills = root->getObject("skills")) {
            for (const auto& kv : *skills) {
                const llvm::json::Object* e = kv.second.getAsObject();
                if (!e) {
                    return indexError(sourceName, "skill entry is not an object");
                }
                SkillEntry entry;
                if (auto t = e->getString("title")) entry.title = t->str();
                if (auto m = e->getString("member")) entry.member = m->str();
                idx.skills_[kv.first.str()] = std::move(entry);
            }
        }
        if (const llvm::json::Object* names = root->getObject("names")) {
            for (const auto& kv : *names) {
                const llvm::json::Array* arr = kv.second.getAsArray();
                if (!arr) {
                    return indexError(sourceName, "name binding is not an array");
                }
                std::vector<std::string> ids;
                for (const auto& v : *arr) {
                    if (auto s = v.getAsString()) ids.push_back(s->str());
                }
                idx.names_[kv.first.str()] = std::move(ids);
            }
        }
        idx.buildSearchStructures();
        return idx;
    }

    std::vector<std::string>
    SkillIndex::query(llvm::StringRef name, bool hierarchical) const {
        std::set<std::string> hits;
        if (auto it = names_.find(name.str()); it != names_.end()) {
            hits.insert(it->second.begin(), it->second.end());
        }
        if (hierarchical) {
            for (const auto& [key, ids] : names_) {
                if (isDescendant(key, name)) {
                    hits.insert(ids.begin(), ids.end());
                }
            }
        }
        return {hits.begin(), hits.end()};
    }

    std::vector<SkillCandidate> SkillIndex::candidates(llvm::StringRef text) const {
        std::set<size_t> hit;
        for (const std::string& g : trigramsOf(text)) {
            if (auto it = trigrams_.find(g); it != trigrams_.end()) {
                hit.insert(it->second.begin(), it->second.end());
            }
        }
        std::vector<SkillCandidate> out;
        out.reserve(hit.size());
        for (size_t i : hit) {
            out.push_back(keys_[i]);
        }
        return out;
    }

    const SkillEntry* SkillIndex::entry(llvm::StringRef id) const {
        auto it = skills_.find(id.str());
        return it == skills_.end() ? nullptr : &it->second;
    }

} // namespace cajeta::buildtool::skill
