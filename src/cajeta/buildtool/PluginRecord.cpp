#include "cajeta/buildtool/PluginRecord.h"

#include <array>

namespace cajeta::buildtool {

    namespace {

        // A required field and the type it must carry, kept as data so the table reads like the spec's.
        enum class FieldType { Str, Int };

        struct Requirement {
            const char* field;
            FieldType type;
        };

        struct KindRule {
            const char* kind;
            std::array<Requirement, 2> required;  // {nullptr,...} terminates
        };

        constexpr Requirement kEnd{nullptr, FieldType::Str};

        const KindRule kRules[] = {
            {"log",     {Requirement{"message", FieldType::Str}, kEnd}},
            {"warn",    {Requirement{"message", FieldType::Str}, kEnd}},
            {"write",   {Requirement{"text",    FieldType::Str}, kEnd}},
            {"output",  {Requirement{"key",     FieldType::Str},
                         Requirement{"value",   FieldType::Str}}},
            {"finding", {Requirement{"severity", FieldType::Str},
                         Requirement{"message",  FieldType::Str}}},
            {"result",  {Requirement{"status",  FieldType::Str}, kEnd}},
            {"error",   {Requirement{"message", FieldType::Str}, kEnd}},
        };

        std::string missing(const char* kind, const char* field) {
            return std::string("record of kind '") + kind + "' is missing required field '"
                 + field + "'";
        }

        std::string wrongType(const char* kind, const char* field, const char* want) {
            return std::string("record of kind '") + kind + "' has field '" + field
                 + "' of the wrong type (expected " + want + ")";
        }

    } // namespace

    RecordCheck checkPluginRecord(const llvm::json::Object& record) {
        auto kind = record.getString("kind");
        if (!kind) {
            return {RecordVerdict::Malformed, "record has no 'kind'"};
        }
        for (const auto& rule : kRules) {
            if (*kind != rule.kind) continue;
            for (const auto& req : rule.required) {
                if (req.field == nullptr) break;
                if (req.type == FieldType::Str) {
                    if (record.find(req.field) == record.end()) {
                        return {RecordVerdict::Malformed, missing(rule.kind, req.field)};
                    }
                    if (!record.getString(req.field)) {
                        return {RecordVerdict::Malformed,
                                wrongType(rule.kind, req.field, "string")};
                    }
                } else {
                    if (record.find(req.field) == record.end()) {
                        return {RecordVerdict::Malformed, missing(rule.kind, req.field)};
                    }
                    if (!record.getInteger(req.field)) {
                        return {RecordVerdict::Malformed,
                                wrongType(rule.kind, req.field, "integer")};
                    }
                }
            }
            return {RecordVerdict::Valid, ""};
        }
        // Well-formed, but not a kind this build knows. A newer plugin may emit kinds
        // we have never heard of, and refusing them would cap every plugin.
        return {RecordVerdict::UnknownKind,
                std::string("unrecognised record kind '") + kind->str() + "'"};
    }

    std::string quoteUntrustedLine(llvm::StringRef line, size_t limit) {
        std::string out;
        out.reserve(line.size() + 8);
        size_t emitted = 0;
        for (size_t i = 0; i < line.size(); ++i) {
            if (emitted >= limit) {
                out += "… (truncated)";
                break;
            }
            unsigned char c = static_cast<unsigned char>(line[i]);
            switch (c) {
                case '\\': out += "\\\\"; emitted += 2; continue;
                case '"':  out += "\\\""; emitted += 2; continue;
                case '\n': out += "\\n";  emitted += 2; continue;
                case '\r': out += "\\r";  emitted += 2; continue;
                case '\t': out += "\\t";  emitted += 2; continue;
                default: break;
            }
            // Control characters and non-printable ASCII are escaped rather than reproduced, so invalid UTF-8 cannot corrupt the output.
            if (c < 0x20 || c >= 0x7f) {
                static const char* hex = "0123456789abcdef";
                out += "\\x";
                out += hex[(c >> 4) & 0xf];
                out += hex[c & 0xf];
                emitted += 4;
                continue;
            }
            out += static_cast<char>(c);
            ++emitted;
        }
        return out;
    }

} // namespace cajeta::buildtool

namespace cajeta::buildtool {

    ConformanceReport checkPluginStream(llvm::ArrayRef<std::string> lines) {
        ConformanceReport report;
        int results = 0;
        for (const auto& raw : lines) {
            llvm::StringRef line(raw);
            line = line.trim();
            if (line.empty()) continue;

            auto parsed = llvm::json::parse(line);
            if (!parsed) {
                llvm::consumeError(parsed.takeError());
                report.problems.push_back(
                    "not valid JSON: \"" + quoteUntrustedLine(line) + "\"");
                continue;
            }
            const llvm::json::Object* obj = parsed->getAsObject();
            if (!obj) {
                report.problems.push_back(
                    "not a JSON object: \"" + quoteUntrustedLine(line) + "\"");
                continue;
            }
            auto check = checkPluginRecord(*obj);
            if (check.verdict == RecordVerdict::Malformed) {
                report.problems.push_back(
                    check.reason + ": \"" + quoteUntrustedLine(line) + "\"");
                continue;
            }
            if (auto kind = obj->getString("kind")) {
                if (*kind == "result") ++results;
            }
        }
        // A plugin that never says how it finished gets one emitted on its behalf (§3), but a conforming plugin still emits exactly one.
        if (results == 0) {
            report.problems.push_back("no result record: the action never "
                                      "reported how it finished");
        } else if (results > 1) {
            report.problems.push_back(
                "emitted " + std::to_string(results) +
                " result records; a completed action reports exactly once");
        }
        report.passed = report.problems.empty();
        return report;
    }

} // namespace cajeta::buildtool
