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

        // Find the position immediately after the opening brace of the
        // object value bound to `key` at the current scan position.
        // Operates on raw text — when the manifest follows the canonical
        // layout (one quoted key, optional whitespace, colon, optional
        // whitespace, opening brace), this lines up. Returns npos when
        // the key isn't followed by an object.
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

        // Given a position immediately inside an object's open brace,
        // return the position of its matching close brace. Skips over
        // nested objects and string contents (with `\"` escape
        // awareness). Returns npos on malformed input.
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

        // True when the substring (open, close) — the *interior* of an
        // object — contains only whitespace.
        bool isEmptyInterior(const std::string& src,
                             size_t openPos, size_t closePos) {
            for (size_t i = openPos; i < closePos; ++i) {
                if (!std::isspace(static_cast<unsigned char>(src[i]))) {
                    return false;
                }
            }
            return true;
        }

        // True when the interior contains at least one entry separator
        // not inside a string at the *current* depth — i.e. the
        // dependency block already has at least one trailing entry
        // we need to follow with a comma.
        bool hasAnyEntry(const std::string& src,
                         size_t openPos, size_t closePos) {
            return !isEmptyInterior(src, openPos, closePos);
        }

        // Validate the candidate output by parsing it through the
        // real manifest loader; returns an error citing the failure
        // when the rewrite produced something invalid.
        llvm::Error validate(const std::string& src) {
            auto m = loadManifestString(src, "<edit-output>");
            if (!m) return m.takeError();
            return llvm::Error::success();
        }

        // Determine the indent (spaces/tabs) used for one level inside
        // an existing object — by looking at the indentation of an
        // existing entry within `(openPos, closePos)`. Falls back to
        // four spaces when the object is empty or we can't tell.
        std::string detectInnerIndent(const std::string& src,
                                      size_t openPos, size_t closePos) {
            // Find the first '\n' inside, then collect leading
            // whitespace of the following line.
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

        // Locate the per-entry key inside an object scope.
        // Returns positions (keyStart, valueEnd) describing the entry
        // span — including the key, colon, value, but NOT any trailing
        // comma or whitespace. Returns nullopt when the key isn't
        // present in this scope.
        struct EntryLocation {
            size_t keyStart;     // position of the opening quote of "name"
            size_t valueEnd;     // position just past the value
            size_t valueStart;   // position of the opening quote of the value
                                  // (or the opening brace for object values)
        };

        // Skip a JSON value starting at `pos` (which must be at the
        // first non-whitespace char of the value). Returns position
        // just past the value. Supports strings, objects, arrays,
        // numbers, true/false/null.
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
            // Bare token: scan until something that ends a value
            // (comma, whitespace, closing brace/bracket).
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
                // Ensure we matched at the start of an entry — i.e.
                // the character before is `{`, `,`, or whitespace.
                if (pos == 0) { scan = pos + needle.size(); continue; }
                char before = src[pos - 1];
                if (before != '{' && before != ',' &&
                    !std::isspace(static_cast<unsigned char>(before))) {
                    scan = pos + needle.size();
                    continue;
                }
                // Validate that the next non-whitespace char is `:`.
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

        // Add the entry `"<key>": "<value>"` to the object whose interior
        // spans (openPos, closePos). Preserves the object's
        // existing indentation style.
        std::string insertEntry(const std::string& src,
                                size_t openPos, size_t closePos,
                                const std::string& key,
                                const std::string& value) {
            std::string entry = "\"" + key + "\": \"" + value + "\"";
            std::string out = src;
            std::string indent = detectInnerIndent(src, openPos, closePos);
            // Outer indent (one level less). Best effort: find the
            // indentation of the line that contains closePos.
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
                // Insert right AFTER the last existing entry's value
                // — that way the trailing "\n<outer-indent>}" the
                // source already had stays intact and the closing
                // brace remains on its own line.
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

        // 1. Try to find settings.dependencies.<name> directly. If
        //    present, rewrite the value in place.
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
                    // Rewrite value in place. The value spans
                    // [valueStart, valueEnd). For our purposes the
                    // value is a quoted string; replace with the new
                    // quoted constraint regardless of the prior shape
                    // (string OR object — this is the same coercion
                    // the parser allows on read).
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
            // Settings exists, no dependencies subobject — add one
            // as an inner entry of settings.
            std::string inner = "{\n}";
            // Insert a "dependencies" entry holding an empty object,
            // then recurse to populate it. Simpler than building the
            // exact text here.
            std::string out = insertEntry(source,
                                          settingsOpen, settingsClose,
                                          "dependencies", "<inline>");
            // The inserted entry came out as
            //   "dependencies": "<inline>"
            // which won't validate. Patch the value to an empty
            // object and re-run via the rewrite path so we get
            // consistent indentation. The marker is unique enough
            // that find() suffices.
            std::string marker = "\"dependencies\": \"<inline>\"";
            auto mp = out.find(marker);
            if (mp == std::string::npos) {
                return err("manifest: failed to inject dependencies "
                           "block (insert marker not found)");
            }
            out.replace(mp, marker.size(),
                        "\"dependencies\": {}");
            // Recurse: now settings.dependencies exists (empty).
            return addDependencyToManifest(out, name, versionConstraint);
        }

        // No settings block at all. Add one with a dependencies
        // subobject. Inject right after the root object's open brace.
        size_t rootOpen = source.find('{');
        if (rootOpen == std::string::npos) {
            return err("manifest: root object not found");
        }
        size_t rootClose = findMatchingClose(source, rootOpen + 1);
        if (rootClose == std::string::npos) {
            return err("manifest: malformed root object");
        }
        // Insert "settings": { "dependencies": { "<name>": "<v>" } }.
        // Use the same insertEntry helper to handle commas/indent.
        std::string scaffold = "{\n    \"dependencies\": {\n        \"" +
                               name + "\": \"" + versionConstraint +
                               "\"\n    }\n}";
        // insertEntry can't directly emit nested objects, so we
        // hand-craft this one. Find the indent style used at the
        // root by looking at any existing entry.
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
        // Walk back from rootClose: if the prior non-whitespace is
        // not `{` and not `,`, we need to add a comma.
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

        // Sibling of findObjectOpenAfterKey for array-valued keys.
        // Returns the position immediately inside the opening `[`.
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

        // Find the position of the matching `]` for an array opened
        // at `insidePos` (the first byte inside `[`).
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

        // Look for the literal entry inside the array bounds only.
        size_t hit = source.find(oldLit, meltsOpen);
        if (hit == std::string::npos || hit >= meltsClose) {
            return err("melt '" + name + "@" + oldVersion +
                       "' not declared in settings.melts");
        }
        // Refuse to rewrite when the literal appears more than once
        // in the array (ambiguity — the user has a duplicate entry,
        // which should be cleaned up by hand before bumping).
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
        // Remove the entry [keyStart, valueEnd) along with any
        // adjacent comma. If there's a comma right after, eat it.
        // Otherwise eat the comma right before (if any).
        std::string out = source;
        size_t removeBegin = entry->keyStart;
        size_t removeEnd = entry->valueEnd;
        // Eat trailing whitespace + comma.
        size_t tail = removeEnd;
        while (tail < depsClose &&
               std::isspace(static_cast<unsigned char>(out[tail]))) ++tail;
        bool ateTrailingComma = false;
        if (tail < depsClose && out[tail] == ',') {
            removeEnd = tail + 1;
            ateTrailingComma = true;
        }
        // If we didn't eat a trailing comma, eat the leading one
        // (we were the last entry).
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

        // Navigate to the `plugins.cajeta.coverage` object's interior
        // bounds. Returns nullopt when any step is missing — the
        // coverage CLI surfaces this as "no cajeta.coverage plugin
        // declared".
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

        // The plugins block accepts a string shorthand
        // (`"cajeta.coverage": "1.0.*"` — the form `cajeta init`'s
        // archetypes ship) as well as the object form findCoverageBlock
        // needs. When the shorthand is present, rewrite it in place to
        // `{ "version": "1.0.*" }` so the exclude editor can proceed —
        // refusing a manifest the plugin resolver itself accepts made
        // `cajeta coverage ignore` unusable on every fresh project.
        // Returns nullopt when no string-form declaration exists.
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

        // Walk an array's interior and yield positions for each
        // object-literal entry. Returns (objectOpen, objectClose)
        // pairs where `objectOpen` is the position just after `{`
        // and `objectClose` is the position of the matching `}`.
        // Non-object array entries (strings — the back-compat form)
        // are skipped here; the caller scans those separately.
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
                // String entry or other — skip it.
                i = skipValue(src, i);
            }
            return out;
        }

        // Read a string-typed field out of an object scope. Used to
        // extract the `kind` and `pattern` of each typed exclude
        // entry. Returns the raw unescaped string contents, or
        // nullopt when the field is missing or its value isn't a
        // string. (We deliberately don't unescape — exclude patterns
        // shouldn't carry JSON escapes in practice, and a literal
        // compare is what the duplicate-check needs anyway.)
        std::optional<std::string> readStringField(
            const std::string& src, size_t openPos, size_t closePos,
            const std::string& field) {
            auto loc = findEntry(src, openPos, closePos, field);
            if (!loc) return std::nullopt;
            if (loc->valueStart >= src.size() ||
                src[loc->valueStart] != '"') {
                return std::nullopt;
            }
            // valueEnd is one past the closing quote (per skipValue).
            // Strip the surrounding quotes.
            if (loc->valueEnd < loc->valueStart + 2) return std::nullopt;
            return src.substr(loc->valueStart + 1,
                              (loc->valueEnd - 1) - (loc->valueStart + 1));
        }

        // Insert (or locate) the `config.exclude` array path inside a
        // coverage block. On entry `covOpen`/`covClose` mark the
        // interior of `plugins.cajeta.coverage`. Mutates `src` to
        // ensure `config` exists (as an object) and `exclude` exists
        // (as an array inside it). Returns the new source plus the
        // array's interior bounds.
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
                // Inject an empty config object.
                std::string out = insertEntry(
                    src, covOpen, covClose, "config", "<inline>");
                std::string marker = "\"config\": \"<inline>\"";
                auto mp = out.find(marker);
                if (mp == std::string::npos) {
                    return err("manifest: failed to inject coverage "
                               "config block (insert marker not found)");
                }
                out.replace(mp, marker.size(), "\"config\": {}");
                // Re-find now that we've mutated the source.
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
                // Inject an empty array.
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

        // Escape a string for safe embedding in a JSON string literal.
        // Handles backslash + double-quote + the common control chars;
        // sufficient for exclude patterns and reason text (which the
        // CLI receives as command-line argv, so it's already plain
        // bytes — we just need to keep the JSON parser happy).
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

        // Duplicate check: scan existing entries for the same kind +
        // pattern. Same pattern with a different reason is still a
        // duplicate — we'd otherwise accumulate stale entries every
        // time the IDE re-runs the action.
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

        // Indent computation. Two shapes to handle:
        //   1. Non-empty array — at least one existing entry on its
        //      own line. Inner indent = the existing entry's indent.
        //   2. Empty / inline array (`[]`) — no inner content to
        //      probe. Fall back to the indent of the line that
        //      contains the array opening, plus one indentation
        //      step. The "indentation step" is detected by reading
        //      the manifest's leading indent style elsewhere in the
        //      source (settings's nesting, root entries).
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
            // Walk the source; whichever indent appears at the
            // first nested line is our step size.
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
            // Outer indent (the array's closing-bracket indent).
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

        // Build the new entry. Always emit kind/pattern/reason in
        // that order so the on-disk shape is predictable for reviewers.
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
            // Append after the last existing entry — same shape as
            // insertEntry but adapted for arrays of objects.
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
            // A string-shorthand declaration can't hold entries; that's
            // "nothing to remove", not an undeclared plugin.
            if (upgradeCoverageShorthand(source)) {
                return err("no exclude entries in cajeta.coverage "
                           "(no config block)");
            }
            return err("no cajeta.coverage plugin declared in plugins");
        }
        // Don't use ensureExcludeArray here — we'd rather error out
        // when there's no exclude array to remove from than silently
        // create one.
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

        // Collect deletion spans first; remove in reverse so earlier
        // offsets stay valid as we splice out later ones.
        struct Span { size_t begin; size_t end; };
        std::vector<Span> toRemove;
        auto entries =
            enumerateObjectEntries(source, arrayOpen, arrayClose);
        for (const auto& [eOpen, eClose] : entries) {
            auto ePat = readStringField(source, eOpen, eClose, "pattern");
            if (!ePat || *ePat != pattern) continue;
            // Entry spans from the `{` (eOpen - 1) through `}` (eClose).
            Span s;
            s.begin = eOpen - 1;
            s.end   = eClose + 1;
            toRemove.push_back(s);
        }
        // Also support the back-compat string-only form: literal
        // `"<pattern>"` entries get nuked too. Scan for them between
        // the array bounds, skipping anything inside an existing
        // object literal (the typed entries we already enumerated).
        std::string strLit = "\"" + pattern + "\"";
        size_t strScan = arrayOpen;
        while (strScan < arrayClose) {
            size_t hit = source.find(strLit, strScan);
            if (hit == std::string::npos || hit >= arrayClose) break;
            // Reject hits that fall inside one of the typed objects.
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
        // Sort by begin so the trailing-comma logic operates on
        // entries in source order.
        std::sort(toRemove.begin(), toRemove.end(),
                  [](const Span& a, const Span& b) {
                      return a.begin < b.begin;
                  });

        std::string out = source;
        // Delete in reverse to keep offsets stable. For each entry,
        // also eat one adjacent comma + the whitespace on whichever
        // side we ate it (so we don't leave dangling commas).
        for (auto it = toRemove.rbegin(); it != toRemove.rend(); ++it) {
            size_t removeBegin = it->begin;
            size_t removeEnd   = it->end;
            // Eat trailing whitespace + comma.
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
            // arrayClose shifted; the next iteration recomputes none
            // of this because we walk in reverse and only the
            // surviving (earlier) spans care about array bounds. The
            // trailing-comma loop above is conservative — it stops
            // at the original arrayClose, which is always still a
            // safe upper bound after deletions.
        }
        if (auto e = validate(out)) return std::move(e);

        RemoveCoverageExcludeResult r;
        r.newSource = std::move(out);
        r.count = static_cast<int>(toRemove.size());
        return r;
    }

} // namespace cajeta::buildtool
