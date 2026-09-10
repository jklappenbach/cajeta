#include "cajeta/buildtool/ManifestEditor.h"

#include "cajeta/buildtool/Manifest.h"

#include <llvm/Support/Error.h>

#include <string>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        // Just inside the opening brace of the object bound to `key`, by raw-text scan.
        size_t findObjectOpenAfterKey(const std::string& src,
                                      size_t fromPos,
                                      const std::string& key) {
            std::string pat = "\"" + key + "\"";
            auto keyPos = src.find(pat, fromPos);
            if (keyPos == std::string::npos) return std::string::npos;
            size_t i = keyPos + pat.size();
            while (i < src.size() &&
                   std::isspace(static_cast<unsigned char>(src[i]))) ++i;
            if (i >= src.size() || src[i] != ':') return std::string::npos;
            ++i;
            while (i < src.size() &&
                   std::isspace(static_cast<unsigned char>(src[i]))) ++i;
            if (i >= src.size() || src[i] != '{') return std::string::npos;
            return i + 1;  // position just inside the open brace
        }

        // The close brace matching an interior that starts at `insidePos`; nested objects
        // and string contents are skipped, and malformed input answers npos.
        size_t findMatchingClose(const std::string& src, size_t insidePos) {
            int depth = 1;
            bool inStr = false;
            bool escape = false;
            for (size_t i = insidePos; i < src.size(); ++i) {
                char c = src[i];
                if (inStr) {
                    if (escape) { escape = false; continue; }
                    if (c == '\\') { escape = true; continue; }
                    if (c == '"') inStr = false;
                    continue;
                }
                if (c == '"') { inStr = true; continue; }
                if (c == '{') ++depth;
                else if (c == '}') {
                    --depth;
                    if (depth == 0) return i;
                }
            }
            return std::string::npos;
        }

        // True when an object's interior is only whitespace.
        bool isEmptyInterior(const std::string& src,
                             size_t openPos, size_t closePos) {
            for (size_t i = openPos; i < closePos; ++i) {
                if (!std::isspace(static_cast<unsigned char>(src[i]))) {
                    return false;
                }
            }
            return true;
        }

        // True when the interior already holds an entry, so the next one needs a comma.
        bool hasAnyEntry(const std::string& src,
                         size_t openPos, size_t closePos) {
            return !isEmptyInterior(src, openPos, closePos);
        }

        // Parses a candidate rewrite through the real manifest loader.
        llvm::Error validate(const std::string& src) {
            auto m = loadManifestString(src, "<edit-output>");
            if (!m) return m.takeError();
            return llvm::Error::success();
        }

        // The indent of one level inside an object, taken from an existing entry there;
        // four spaces when the object is empty or nothing can be read.
        std::string detectInnerIndent(const std::string& src,
                                      size_t openPos, size_t closePos) {
            for (size_t i = openPos; i < closePos; ++i) {
                if (src[i] == '\n') {
                    size_t j = i + 1;
                    std::string lead;
                    while (j < closePos &&
                           (src[j] == ' ' || src[j] == '\t')) {
                        lead += src[j];
                        ++j;
                    }
                    if (!lead.empty()) return lead;
                }
            }
            return "    ";
        }

        // One entry's span in an object scope: key, colon and value, no trailing comma.
        struct EntryLocation {
            size_t keyStart;     // the key's opening quote
            size_t valueEnd;     // just past the value
            size_t valueStart;   // the value's first char: a quote, or a brace
        };

        // The position just past the JSON value starting at `pos`, which must be that
        // value's first non-whitespace char. Strings, objects, arrays and bare tokens.
        size_t skipValue(const std::string& src, size_t pos) {
            if (pos >= src.size()) return pos;
            char c = src[pos];
            if (c == '"') {
                bool escape = false;
                for (size_t i = pos + 1; i < src.size(); ++i) {
                    if (escape) { escape = false; continue; }
                    if (src[i] == '\\') { escape = true; continue; }
                    if (src[i] == '"') return i + 1;
                }
                return src.size();
            }
            if (c == '{' || c == '[') {
                char close = (c == '{') ? '}' : ']';
                int depth = 1;
                bool inStr = false;
                bool escape = false;
                for (size_t i = pos + 1; i < src.size(); ++i) {
                    char ch = src[i];
                    if (inStr) {
                        if (escape) { escape = false; continue; }
                        if (ch == '\\') { escape = true; continue; }
                        if (ch == '"') inStr = false;
                        continue;
                    }
                    if (ch == '"') { inStr = true; continue; }
                    if (ch == c) ++depth;
                    else if (ch == close) {
                        --depth;
                        if (depth == 0) return i + 1;
                    }
                }
                return src.size();
            }
            // Bare token: scan to a comma, whitespace or closing brace or bracket.
            for (size_t i = pos; i < src.size(); ++i) {
                char ch = src[i];
                if (ch == ',' || ch == '}' || ch == ']' ||
                    std::isspace(static_cast<unsigned char>(ch))) {
                    return i;
                }
            }
            return src.size();
        }

        std::optional<EntryLocation> findEntry(
            const std::string& src,
            size_t openPos, size_t closePos,
            const std::string& key) {
            std::string needle = "\"" + key + "\"";
            size_t scan = openPos;
            while (scan < closePos) {
                auto pos = src.find(needle, scan);
                if (pos == std::string::npos || pos >= closePos) {
                    return std::nullopt;
                }
                // Only a match at an entry start counts: `{`, `,` or whitespace before.
                if (pos == 0) { scan = pos + needle.size(); continue; }
                char before = src[pos - 1];
                if (before != '{' && before != ',' &&
                    !std::isspace(static_cast<unsigned char>(before))) {
                    scan = pos + needle.size();
                    continue;
                }
                size_t i = pos + needle.size();
                while (i < closePos &&
                       std::isspace(static_cast<unsigned char>(src[i]))) ++i;
                if (i >= closePos || src[i] != ':') {
                    scan = pos + needle.size();
                    continue;
                }
                ++i;  // past ':'
                while (i < closePos &&
                       std::isspace(static_cast<unsigned char>(src[i]))) ++i;
                EntryLocation loc;
                loc.keyStart = pos;
                loc.valueStart = i;
                loc.valueEnd = skipValue(src, i);
                return loc;
            }
            return std::nullopt;
        }

        // Adds `"<key>": "<value>"` to the object whose interior spans (openPos,
        // closePos), preserving that object's indentation style.
        std::string insertEntry(const std::string& src,
                                size_t openPos, size_t closePos,
                                const std::string& key,
                                const std::string& value) {
            std::string entry = "\"" + key + "\": \"" + value + "\"";
            std::string out = src;
            std::string indent = detectInnerIndent(src, openPos, closePos);
            std::string outer;
            for (size_t i = closePos; i-- > 0; ) {
                if (out[i] == '\n') {
                    size_t j = i + 1;
                    while (j < closePos &&
                           (out[j] == ' ' || out[j] == '\t')) {
                        outer += out[j];
                        ++j;
                    }
                    break;
                }
            }

            if (isEmptyInterior(out, openPos, closePos)) {
                std::string injected = std::string("\n") + indent + entry +
                                       "\n" + outer;
                out.replace(openPos, closePos - openPos, injected);
            } else {
                // After the last entry's value, so the source's own trailing newline
                // and closing brace stay where they were.
                size_t prev = closePos;
                while (prev > openPos &&
                       std::isspace(static_cast<unsigned char>(out[prev - 1]))) {
                    --prev;
                }
                std::string injection;
                if (prev > openPos && out[prev - 1] != ',') {
                    injection += ",";
                }
                injection += "\n";
                injection += indent;
                injection += entry;
                out.insert(prev, injection);
            }
            return out;
        }

    } // namespace

    llvm::Expected<std::string> addDependencyToManifest(
        const std::string& source,
        const std::string& name,
        const std::string& versionConstraint) {

        // Validate first so we don't write garbage on top of garbage.
        if (auto e = validate(source)) return std::move(e);

        // settings.dependencies.<name> present: rewrite the value in place.
        size_t settingsOpen = findObjectOpenAfterKey(source, 0, "settings");
        if (settingsOpen != std::string::npos) {
            size_t settingsClose =
                findMatchingClose(source, settingsOpen);
            if (settingsClose == std::string::npos) {
                return err("manifest: malformed settings block");
            }
            size_t depsOpen = findObjectOpenAfterKey(
                source, settingsOpen, "dependencies");
            if (depsOpen != std::string::npos && depsOpen < settingsClose) {
                size_t depsClose = findMatchingClose(source, depsOpen);
                if (depsClose == std::string::npos) {
                    return err("manifest: malformed dependencies block");
                }
                if (auto entry = findEntry(source, depsOpen, depsClose,
                                           name)) {
                    // Whatever the prior shape, the replacement is a quoted constraint.
                    std::string out = source;
                    std::string repl = "\"" + versionConstraint + "\"";
                    out.replace(entry->valueStart,
                                entry->valueEnd - entry->valueStart,
                                repl);
                    if (auto e = validate(out)) return std::move(e);
                    return out;
                }
                // Append to existing dependencies block.
                std::string out = insertEntry(source, depsOpen, depsClose,
                                              name, versionConstraint);
                if (auto e = validate(out)) return std::move(e);
                return out;
            }
            // Settings without dependencies: insert an empty one, then recurse into it.
            std::string inner = "{\n}";
            std::string out = insertEntry(source,
                                          settingsOpen, settingsClose,
                                          "dependencies", "<inline>");
            // The inserted string entry would not validate: patch it to an empty object.
            std::string marker = "\"dependencies\": \"<inline>\"";
            auto mp = out.find(marker);
            if (mp == std::string::npos) {
                return err("manifest: failed to inject dependencies "
                           "block (insert marker not found)");
            }
            out.replace(mp, marker.size(),
                        "\"dependencies\": {}");
            return addDependencyToManifest(out, name, versionConstraint);
        }

        // No settings block: add one, with its dependencies subobject, after `{`.
        size_t rootOpen = source.find('{');
        if (rootOpen == std::string::npos) {
            return err("manifest: root object not found");
        }
        size_t rootClose = findMatchingClose(source, rootOpen + 1);
        if (rootClose == std::string::npos) {
            return err("manifest: malformed root object");
        }
        // Insert "settings": { "dependencies": { "<name>": "<v>" } }.
        std::string scaffold = "{\n    \"dependencies\": {\n        \"" +
                               name + "\": \"" + versionConstraint +
                               "\"\n    }\n}";
        // Hand-crafted, since insertEntry cannot emit nested objects.
        std::string outerIndent;
        for (size_t i = rootOpen + 1; i < rootClose; ++i) {
            if (source[i] == '\n') {
                size_t j = i + 1;
                while (j < rootClose &&
                       (source[j] == ' ' || source[j] == '\t')) {
                    outerIndent += source[j];
                    ++j;
                }
                if (!outerIndent.empty()) break;
            }
        }
        if (outerIndent.empty()) outerIndent = "    ";
        std::string innerIndent = outerIndent + outerIndent;
        std::string entryText = outerIndent + "\"settings\": {\n" +
                                innerIndent + "\"dependencies\": {\n" +
                                innerIndent + outerIndent + "\"" +
                                name + "\": \"" + versionConstraint +
                                "\"\n" + innerIndent + "}\n" +
                                outerIndent + "}";
        std::string out = source;
        // A comma is needed unless the prior non-whitespace is `{` or `,`.
        size_t prev = rootClose;
        while (prev > rootOpen + 1 &&
               std::isspace(static_cast<unsigned char>(out[prev - 1]))) {
            --prev;
        }
        std::string injection;
        if (prev > rootOpen + 1 && out[prev - 1] != ',' &&
            out[prev - 1] != '{') {
            injection += ",";
        }
        injection += "\n";
        injection += entryText;
        injection += "\n";
        out.insert(rootClose, injection);
        if (auto e = validate(out)) return std::move(e);
        return out;
    }

    namespace {

        // findObjectOpenAfterKey's sibling for array-valued keys: just inside the `[`.
        size_t findArrayOpenAfterKey(const std::string& src,
                                     size_t fromPos,
                                     const std::string& key) {
            std::string pat = "\"" + key + "\"";
            auto keyPos = src.find(pat, fromPos);
            if (keyPos == std::string::npos) return std::string::npos;
            size_t i = keyPos + pat.size();
            while (i < src.size() &&
                   std::isspace(static_cast<unsigned char>(src[i]))) ++i;
            if (i >= src.size() || src[i] != ':') return std::string::npos;
            ++i;
            while (i < src.size() &&
                   std::isspace(static_cast<unsigned char>(src[i]))) ++i;
            if (i >= src.size() || src[i] != '[') return std::string::npos;
            return i + 1;
        }

        // The matching `]` for an array whose interior starts at `insidePos`.
        size_t findMatchingArrayClose(const std::string& src,
                                      size_t insidePos) {
            int depth = 1;
            bool inStr = false;
            bool escape = false;
            for (size_t i = insidePos; i < src.size(); ++i) {
                char c = src[i];
                if (inStr) {
                    if (escape) { escape = false; continue; }
                    if (c == '\\') { escape = true; continue; }
                    if (c == '"') inStr = false;
                    continue;
                }
                if (c == '"') { inStr = true; continue; }
                if (c == '[') ++depth;
                else if (c == ']') {
                    --depth;
                    if (depth == 0) return i;
                }
            }
            return std::string::npos;
        }

    } // namespace

    llvm::Expected<std::string> setMeltImportInManifest(
        const std::string& source,
        const std::string& name,
        const std::string& oldVersion,
        const std::string& newVersion) {
        if (auto e = validate(source)) return std::move(e);

        size_t settingsOpen = findObjectOpenAfterKey(source, 0, "settings");
        if (settingsOpen == std::string::npos) {
            return err("melt '" + name + "@" + oldVersion +
                       "' not declared (no settings.melts block)");
        }
        size_t settingsClose = findMatchingClose(source, settingsOpen);
        if (settingsClose == std::string::npos) {
            return err("manifest: malformed settings block");
        }
        size_t meltsOpen =
            findArrayOpenAfterKey(source, settingsOpen, "melts");
        if (meltsOpen == std::string::npos || meltsOpen >= settingsClose) {
            return err("melt '" + name + "@" + oldVersion +
                       "' not declared (no settings.melts array)");
        }
        size_t meltsClose = findMatchingArrayClose(source, meltsOpen);
        if (meltsClose == std::string::npos) {
            return err("manifest: malformed settings.melts array");
        }

        std::string oldLit = "\"" + name + "@" + oldVersion + "\"";
        std::string newLit = "\"" + name + "@" + newVersion + "\"";

        size_t hit = source.find(oldLit, meltsOpen);
        if (hit == std::string::npos || hit >= meltsClose) {
            return err("melt '" + name + "@" + oldVersion +
                       "' not declared in settings.melts");
        }
        // A duplicated literal is ambiguous: it must be cleaned up by hand first.
        size_t second = source.find(oldLit, hit + oldLit.size());
        if (second != std::string::npos && second < meltsClose) {
            return err("melt '" + name + "@" + oldVersion +
                       "' appears more than once in settings.melts — "
                       "deduplicate before upgrading");
        }

        std::string out = source;
        out.replace(hit, oldLit.size(), newLit);
        if (auto e = validate(out)) return std::move(e);
        return out;
    }

    llvm::Expected<std::string> removeDependencyFromManifest(
        const std::string& source,
        const std::string& name) {
        if (auto e = validate(source)) return std::move(e);

        size_t settingsOpen = findObjectOpenAfterKey(source, 0, "settings");
        if (settingsOpen == std::string::npos) {
            return err("'" + name +
                       "' is not declared (no settings.dependencies block)");
        }
        size_t settingsClose = findMatchingClose(source, settingsOpen);
        if (settingsClose == std::string::npos) {
            return err("manifest: malformed settings block");
        }
        size_t depsOpen = findObjectOpenAfterKey(source, settingsOpen,
                                                 "dependencies");
        if (depsOpen == std::string::npos || depsOpen >= settingsClose) {
            return err("'" + name +
                       "' is not declared (no settings.dependencies block)");
        }
        size_t depsClose = findMatchingClose(source, depsOpen);
        auto entry = findEntry(source, depsOpen, depsClose, name);
        if (!entry) {
            return err("'" + name + "' is not declared in "
                       "settings.dependencies");
        }
        // Remove the entry with one adjacent comma: the following one, else the leading.
        std::string out = source;
        size_t removeBegin = entry->keyStart;
        size_t removeEnd = entry->valueEnd;
        size_t tail = removeEnd;
        while (tail < depsClose &&
               std::isspace(static_cast<unsigned char>(out[tail]))) ++tail;
        bool ateTrailingComma = false;
        if (tail < depsClose && out[tail] == ',') {
            removeEnd = tail + 1;
            ateTrailingComma = true;
        }
        if (!ateTrailingComma) {
            ssize_t head = static_cast<ssize_t>(removeBegin) - 1;
            while (head >= static_cast<ssize_t>(depsOpen) &&
                   std::isspace(
                       static_cast<unsigned char>(out[head]))) --head;
            if (head >= static_cast<ssize_t>(depsOpen) &&
                out[head] == ',') {
                removeBegin = static_cast<size_t>(head);
            }
        }
        out.erase(removeBegin, removeEnd - removeBegin);
        if (auto e = validate(out)) return std::move(e);
        return out;
    }

    namespace {

        // The interior bounds of `plugins.cajeta.coverage`; nullopt when a step is missing.
        struct CoverageBlockBounds {
            size_t pluginOpen;    // interior of plugins.cajeta.coverage
            size_t pluginClose;
        };
        std::optional<CoverageBlockBounds>
        findCoverageBlock(const std::string& src) {
            size_t pluginsOpen = findObjectOpenAfterKey(src, 0, "plugins");
            if (pluginsOpen == std::string::npos) return std::nullopt;
            size_t pluginsClose = findMatchingClose(src, pluginsOpen);
            if (pluginsClose == std::string::npos) return std::nullopt;
            size_t covOpen = findObjectOpenAfterKey(
                src, pluginsOpen, "cajeta.coverage");
            if (covOpen == std::string::npos || covOpen >= pluginsClose) {
                return std::nullopt;
            }
            size_t covClose = findMatchingClose(src, covOpen);
            if (covClose == std::string::npos) return std::nullopt;
            CoverageBlockBounds b;
            b.pluginOpen = covOpen;
            b.pluginClose = covClose;
            return b;
        }

        // Rewrites the string shorthand `"cajeta.coverage": "1.0.*"`, which the
        // archetypes ship and the resolver accepts, into the object form the exclude
        // editor needs. nullopt when no string-form declaration exists.
        std::optional<std::string> upgradeCoverageShorthand(
            const std::string& src) {
            size_t pluginsOpen = findObjectOpenAfterKey(src, 0, "plugins");
            if (pluginsOpen == std::string::npos) return std::nullopt;
            size_t pluginsClose = findMatchingClose(src, pluginsOpen);
            if (pluginsClose == std::string::npos) return std::nullopt;
            size_t keyPos = src.find("\"cajeta.coverage\"", pluginsOpen);
            if (keyPos == std::string::npos || keyPos >= pluginsClose) {
                return std::nullopt;
            }
            size_t colon = src.find(':', keyPos + 17);
            if (colon == std::string::npos) return std::nullopt;
            size_t v = colon + 1;
            while (v < src.size() &&
                   std::isspace(static_cast<unsigned char>(src[v]))) ++v;
            if (v >= src.size() || src[v] != '"') return std::nullopt;
            size_t vEnd = src.find('"', v + 1);
            if (vEnd == std::string::npos) return std::nullopt;
            std::string out = src;
            out.replace(v, vEnd - v + 1,
                        "{ \"version\": " + src.substr(v, vEnd - v + 1) + " }");
            return out;
        }

        // The (interior, close) pair of every object-literal entry in an array; the
        // back-compat string entries are skipped for the caller to scan separately.
        std::vector<std::pair<size_t, size_t>>
        enumerateObjectEntries(const std::string& src,
                               size_t openPos, size_t closePos) {
            std::vector<std::pair<size_t, size_t>> out;
            size_t i = openPos;
            while (i < closePos) {
                while (i < closePos &&
                       (std::isspace(static_cast<unsigned char>(src[i])) ||
                        src[i] == ',')) ++i;
                if (i >= closePos) break;
                if (src[i] == '{') {
                    size_t inside = i + 1;
                    size_t close = findMatchingClose(src, inside);
                    if (close == std::string::npos || close >= closePos) break;
                    out.emplace_back(inside, close);
                    i = close + 1;
                    continue;
                }
                i = skipValue(src, i);
            }
            return out;
        }

        // A string field's contents from an object scope, or nullopt when it is missing
        // or not a string. Deliberately not unescaped: the duplicate check compares raw.
        std::optional<std::string> readStringField(
            const std::string& src, size_t openPos, size_t closePos,
            const std::string& field) {
            auto loc = findEntry(src, openPos, closePos, field);
            if (!loc) return std::nullopt;
            if (loc->valueStart >= src.size() ||
                src[loc->valueStart] != '"') {
                return std::nullopt;
            }
            // valueEnd is one past the closing quote, so strip both quotes.
            if (loc->valueEnd < loc->valueStart + 2) return std::nullopt;
            return src.substr(loc->valueStart + 1,
                              (loc->valueEnd - 1) - (loc->valueStart + 1));
        }

        // Ensures `config` and its `exclude` array exist in the coverage block bounded by
        // covOpen/covClose; answers the rewritten source and the array's interior bounds.
        struct EnsuredExclude {
            std::string source;
            size_t arrayOpen;
            size_t arrayClose;
        };
        llvm::Expected<EnsuredExclude> ensureExcludeArray(
            const std::string& srcIn,
            size_t covOpen, size_t covClose) {
            std::string src = srcIn;

            size_t configOpen = findObjectOpenAfterKey(
                src, covOpen, "config");
            if (configOpen == std::string::npos || configOpen >= covClose) {
                std::string out = insertEntry(
                    src, covOpen, covClose, "config", "<inline>");
                std::string marker = "\"config\": \"<inline>\"";
                auto mp = out.find(marker);
                if (mp == std::string::npos) {
                    return err("manifest: failed to inject coverage "
                               "config block (insert marker not found)");
                }
                out.replace(mp, marker.size(), "\"config\": {}");
                // Every offset must be re-found now that the source has moved.
                src = out;
                covOpen = findObjectOpenAfterKey(src, 0, "cajeta.coverage");
                if (covOpen == std::string::npos) {
                    return err("manifest: coverage block lost after "
                               "config injection");
                }
                covClose = findMatchingClose(src, covOpen);
                configOpen = findObjectOpenAfterKey(
                    src, covOpen, "config");
                if (configOpen == std::string::npos) {
                    return err("manifest: config block lost after "
                               "injection");
                }
            }
            size_t configClose = findMatchingClose(src, configOpen);
            if (configClose == std::string::npos) {
                return err("manifest: malformed coverage config block");
            }

            size_t arrayOpen = findArrayOpenAfterKey(
                src, configOpen, "exclude");
            if (arrayOpen == std::string::npos || arrayOpen >= configClose) {
                std::string out = insertEntry(
                    src, configOpen, configClose, "exclude", "<inline>");
                std::string marker = "\"exclude\": \"<inline>\"";
                auto mp = out.find(marker);
                if (mp == std::string::npos) {
                    return err("manifest: failed to inject coverage "
                               "exclude array (insert marker not found)");
                }
                out.replace(mp, marker.size(), "\"exclude\": []");
                src = out;
                covOpen = findObjectOpenAfterKey(src, 0, "cajeta.coverage");
                covClose = findMatchingClose(src, covOpen);
                configOpen = findObjectOpenAfterKey(src, covOpen, "config");
                configClose = findMatchingClose(src, configOpen);
                arrayOpen = findArrayOpenAfterKey(
                    src, configOpen, "exclude");
                if (arrayOpen == std::string::npos) {
                    return err("manifest: exclude array lost after "
                               "injection");
                }
            }
            size_t arrayClose = findMatchingArrayClose(src, arrayOpen);
            if (arrayClose == std::string::npos) {
                return err("manifest: malformed coverage exclude array");
            }

            EnsuredExclude r;
            r.source = std::move(src);
            r.arrayOpen = arrayOpen;
            r.arrayClose = arrayClose;
            return r;
        }

        // Escapes backslash, double quote and the common control chars — enough for the
        // plain argv bytes an exclude pattern or reason arrives as.
        std::string jsonEscape(const std::string& s) {
            std::string out;
            out.reserve(s.size() + 2);
            for (char c : s) {
                if (c == '\\') { out += "\\\\"; }
                else if (c == '"') { out += "\\\""; }
                else if (c == '\n') { out += "\\n"; }
                else if (c == '\r') { out += "\\r"; }
                else if (c == '\t') { out += "\\t"; }
                else { out += c; }
            }
            return out;
        }

    } // namespace

    llvm::Expected<std::string> appendCoverageExclude(
        const std::string& source,
        const std::string& kind,
        const std::string& pattern,
        const std::string& reason) {
        if (auto e = validate(source)) return std::move(e);

        if (kind != "file" && kind != "package" && kind != "symbol") {
            return err("appendCoverageExclude: kind must be one of "
                       "'file', 'package', 'symbol' (got '" + kind + "')");
        }

        std::string upgraded;
        const std::string* effective = &source;
        auto bounds = findCoverageBlock(source);
        if (!bounds) {
            if (auto up = upgradeCoverageShorthand(source)) {
                upgraded = std::move(*up);
                bounds = findCoverageBlock(upgraded);
                if (bounds) effective = &upgraded;
            }
        }
        if (!bounds) {
            return err("no cajeta.coverage plugin declared in "
                       "plugins; add it before calling "
                       "`cajeta coverage ignore`");
        }

        auto ensured = ensureExcludeArray(*effective,
                                          bounds->pluginOpen,
                                          bounds->pluginClose);
        if (!ensured) return ensured.takeError();

        // A different reason is still a duplicate, or IDE re-runs would accumulate.
        std::string src = std::move(ensured->source);
        size_t arrayOpen = ensured->arrayOpen;
        size_t arrayClose = ensured->arrayClose;
        auto entries = enumerateObjectEntries(src, arrayOpen, arrayClose);
        for (const auto& [eOpen, eClose] : entries) {
            auto eKind = readStringField(src, eOpen, eClose, "kind");
            auto ePat  = readStringField(src, eOpen, eClose, "pattern");
            if (eKind && ePat && *eKind == kind && *ePat == pattern) {
                return err("coverage exclude already present: " +
                           kind + " '" + pattern + "'");
            }
        }

        // A non-empty array takes its entries' indent; an empty or inline one has
        // nothing to probe, so it uses its own line's indent plus one step.
        auto leadingIndentOnLineContaining =
            [&](size_t pos) -> std::string {
                if (pos > src.size()) return std::string();
                size_t lineStart = pos;
                while (lineStart > 0 && src[lineStart - 1] != '\n') {
                    --lineStart;
                }
                std::string lead;
                size_t j = lineStart;
                while (j < src.size() &&
                       (src[j] == ' ' || src[j] == '\t')) {
                    lead += src[j];
                    ++j;
                }
                return lead;
            };
        auto detectOneIndentStep = [&]() -> std::string {
            for (size_t i = 0; i + 1 < src.size(); ++i) {
                if (src[i] != '\n') continue;
                size_t j = i + 1;
                std::string lead;
                while (j < src.size() &&
                       (src[j] == ' ' || src[j] == '\t')) {
                    lead += src[j];
                    ++j;
                }
                if (!lead.empty()) return lead;
            }
            return "    ";
        };

        std::string outer;
        std::string indent;
        if (isEmptyInterior(src, arrayOpen, arrayClose)) {
            outer  = leadingIndentOnLineContaining(arrayOpen);
            indent = outer + detectOneIndentStep();
        } else {
            indent = detectInnerIndent(src, arrayOpen, arrayClose);
            for (size_t i = arrayClose; i-- > 0; ) {
                if (src[i] == '\n') {
                    size_t j = i + 1;
                    while (j < arrayClose &&
                           (src[j] == ' ' || src[j] == '\t')) {
                        outer += src[j];
                        ++j;
                    }
                    break;
                }
            }
        }
        std::string innerIndent = indent + detectOneIndentStep();

        // Always kind, pattern, reason in that order: the on-disk shape is reviewable.
        std::string entry =
            "{\n" +
            innerIndent + "\"kind\": \""    + jsonEscape(kind)    + "\",\n" +
            innerIndent + "\"pattern\": \"" + jsonEscape(pattern) + "\",\n" +
            innerIndent + "\"reason\": \""  + jsonEscape(reason)  + "\"\n" +
            indent + "}";

        std::string out = src;
        if (isEmptyInterior(out, arrayOpen, arrayClose)) {
            std::string injected = "\n" + indent + entry + "\n" + outer;
            out.replace(arrayOpen, arrayClose - arrayOpen, injected);
        } else {
            // After the last entry, as insertEntry does, but for an array of objects.
            size_t prev = arrayClose;
            while (prev > arrayOpen &&
                   std::isspace(static_cast<unsigned char>(out[prev - 1]))) {
                --prev;
            }
            std::string injection;
            if (prev > arrayOpen && out[prev - 1] != ',') {
                injection += ",";
            }
            injection += "\n";
            injection += indent;
            injection += entry;
            out.insert(prev, injection);
        }
        if (auto e = validate(out)) return std::move(e);
        return out;
    }

    llvm::Expected<RemoveCoverageExcludeResult> removeCoverageExclude(
        const std::string& source,
        const std::string& pattern) {
        if (auto e = validate(source)) return std::move(e);

        auto bounds = findCoverageBlock(source);
        if (!bounds) {
            // A string shorthand holds no entries: nothing to remove, not undeclared.
            if (upgradeCoverageShorthand(source)) {
                return err("no exclude entries in cajeta.coverage "
                           "(no config block)");
            }
            return err("no cajeta.coverage plugin declared in plugins");
        }
        // Not ensureExcludeArray: with nothing to remove from, this should error.
        size_t configOpen = findObjectOpenAfterKey(
            source, bounds->pluginOpen, "config");
        if (configOpen == std::string::npos ||
            configOpen >= bounds->pluginClose) {
            return err("no exclude entries in cajeta.coverage "
                       "(no config block)");
        }
        size_t configClose = findMatchingClose(source, configOpen);
        size_t arrayOpen = findArrayOpenAfterKey(
            source, configOpen, "exclude");
        if (arrayOpen == std::string::npos || arrayOpen >= configClose) {
            return err("no exclude entries in cajeta.coverage "
                       "(no exclude array)");
        }
        size_t arrayClose = findMatchingArrayClose(source, arrayOpen);
        if (arrayClose == std::string::npos) {
            return err("manifest: malformed coverage exclude array");
        }

        // Collect the spans first and remove in reverse, so earlier offsets stay valid.
        struct Span { size_t begin; size_t end; };
        std::vector<Span> toRemove;
        auto entries =
            enumerateObjectEntries(source, arrayOpen, arrayClose);
        for (const auto& [eOpen, eClose] : entries) {
            auto ePat = readStringField(source, eOpen, eClose, "pattern");
            if (!ePat || *ePat != pattern) continue;
            Span s;
            s.begin = eOpen - 1;
            s.end   = eClose + 1;
            toRemove.push_back(s);
        }
        // Back-compat string entries go too, minus hits inside an enumerated object.
        std::string strLit = "\"" + pattern + "\"";
        size_t strScan = arrayOpen;
        while (strScan < arrayClose) {
            size_t hit = source.find(strLit, strScan);
            if (hit == std::string::npos || hit >= arrayClose) break;
            bool insideObj = false;
            for (const auto& [eOpen, eClose] : entries) {
                if (hit >= eOpen - 1 && hit <= eClose + 1) {
                    insideObj = true;
                    break;
                }
            }
            if (!insideObj) {
                Span s;
                s.begin = hit;
                s.end   = hit + strLit.size();
                toRemove.push_back(s);
            }
            strScan = hit + strLit.size();
        }
        if (toRemove.empty()) {
            return err("coverage exclude '" + pattern +
                       "' not found");
        }
        // Sorted by begin so the trailing-comma logic sees source order.
        std::sort(toRemove.begin(), toRemove.end(),
                  [](const Span& a, const Span& b) {
                      return a.begin < b.begin;
                  });

        std::string out = source;
        // In reverse, eating one adjacent comma so no dangling comma is left behind.
        for (auto it = toRemove.rbegin(); it != toRemove.rend(); ++it) {
            size_t removeBegin = it->begin;
            size_t removeEnd   = it->end;
            size_t tail = removeEnd;
            while (tail < arrayClose &&
                   std::isspace(static_cast<unsigned char>(out[tail]))) {
                ++tail;
            }
            bool ateTrailing = false;
            if (tail < arrayClose && out[tail] == ',') {
                removeEnd = tail + 1;
                ateTrailing = true;
            }
            if (!ateTrailing) {
                ssize_t head = static_cast<ssize_t>(removeBegin) - 1;
                while (head >= static_cast<ssize_t>(arrayOpen) &&
                       std::isspace(
                           static_cast<unsigned char>(out[head]))) --head;
                if (head >= static_cast<ssize_t>(arrayOpen) &&
                    out[head] == ',') {
                    removeBegin = static_cast<size_t>(head);
                }
            }
            out.erase(removeBegin, removeEnd - removeBegin);
            // arrayClose is stale but still an upper bound; the reverse walk keeps every
            // surviving span earlier than this erasure.
        }
        if (auto e = validate(out)) return std::move(e);

        RemoveCoverageExcludeResult r;
        r.newSource = std::move(out);
        r.count = static_cast<int>(toRemove.size());
        return r;
    }

} // namespace cajeta::buildtool
