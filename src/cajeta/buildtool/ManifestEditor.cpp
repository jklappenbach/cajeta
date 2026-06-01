#include "cajeta/buildtool/ManifestEditor.h"

#include "cajeta/buildtool/Manifest.h"

#include <llvm/Support/Error.h>

#include <regex>
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

} // namespace cajeta::buildtool
