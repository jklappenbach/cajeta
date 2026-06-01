// `version` — semver bump / set; writes back to the manifest.
//
// Params:
//   bump        (one of) "major" | "minor" | "patch"   OR
//   set         (one of) "<semver>"
//   write-to    (optional, default "./cajeta.json")
//
// Outputs:
//   version     the new version
//   previous    the old version

#include "cajeta/buildtool/Action.h"

#include <llvm/Support/Error.h>

#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        struct Semver {
            unsigned major = 0;
            unsigned minor = 0;
            unsigned patch = 0;
            std::string prerelease;   // "rc1" etc.
            std::string buildMeta;    // "+sha.123"
            std::string toString() const {
                std::ostringstream os;
                os << major << '.' << minor << '.' << patch;
                if (!prerelease.empty()) os << '-' << prerelease;
                if (!buildMeta.empty())  os << '+' << buildMeta;
                return os.str();
            }
        };

        llvm::Expected<Semver> parseSemver(const std::string& s) {
            // MAJOR.MINOR.PATCH[-prerelease][+build]
            std::regex re(R"(^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?(?:\+([0-9A-Za-z.-]+))?$)");
            std::smatch m;
            if (!std::regex_match(s, m, re)) {
                return err("version: not a valid semver: '" + s + "'");
            }
            Semver v;
            v.major = static_cast<unsigned>(std::stoul(m[1].str()));
            v.minor = static_cast<unsigned>(std::stoul(m[2].str()));
            v.patch = static_cast<unsigned>(std::stoul(m[3].str()));
            v.prerelease = m[4].matched ? m[4].str() : "";
            v.buildMeta  = m[5].matched ? m[5].str() : "";
            return v;
        }

        // Find the `"version"` field inside the `"details"` block of
        // a JSONC source and replace its value with `newVersion`.
        // Returns the previous value via `previous`. Preserves the
        // rest of the file byte-for-byte (including comments,
        // formatting, trailing commas).
        //
        // Match is best-effort by regex: looks for the *first*
        // `"version"\s*:\s*"..."` after the *first* `"details"` key.
        // Sufficient for the canonical manifest shape; more general
        // edits would need a JSONC editor (out of scope here).
        llvm::Expected<std::string> rewriteVersionInPlace(
            std::string& src,
            const std::string& newVersion) {
            auto detailsPos = src.find("\"details\"");
            if (detailsPos == std::string::npos) {
                return err("version: manifest has no \"details\" block");
            }
            // Look for the version field after that.
            std::regex re(R"(("version"\s*:\s*")([^"]*)("))");
            std::smatch m;
            auto begin = src.cbegin() + static_cast<long>(detailsPos);
            if (!std::regex_search(begin, src.cend(), m, re)) {
                return err("version: no version field found after "
                           "\"details\" in the manifest");
            }
            std::string previous = m[2].str();
            // Replace the version value in place.
            auto matchStart = static_cast<size_t>(m.position(2) +
                              (begin - src.cbegin()));
            auto matchLen   = static_cast<size_t>(m.length(2));
            src.replace(matchStart, matchLen, newVersion);
            return previous;
        }

    } // namespace

    class VersionAction : public Action {
    public:
        std::string name() const override { return "version"; }

        llvm::Expected<ActionResult> run(
            const llvm::json::Object& params,
            TaskContext& /*ctx*/) const override {

            std::string writeTo = "./cajeta.json";
            if (auto v = params.getString("write-to")) writeTo = v->str();

            // Read the manifest source.
            std::string src;
            {
                std::ifstream in(writeTo);
                if (!in) return err("version: cannot read manifest '" +
                                    writeTo + "'");
                std::ostringstream ss; ss << in.rdbuf();
                src = ss.str();
            }

            // Determine the new version.
            std::string newVersionStr;
            auto bump = params.getString("bump");
            auto setV = params.getString("set");
            if (bump && setV) {
                return err("version: provide either 'bump' OR 'set', "
                           "not both");
            }
            if (setV) {
                auto sv = parseSemver(setV->str());
                if (!sv) return sv.takeError();
                newVersionStr = sv->toString();
            } else if (bump) {
                // We need the current version to bump from. Pull it
                // from the manifest by doing a no-op rewrite and
                // capturing the previous value, then re-rewriting.
                std::string scratch = src;
                auto prev = rewriteVersionInPlace(scratch, "<probe>");
                if (!prev) return prev.takeError();
                auto sv = parseSemver(*prev);
                if (!sv) return sv.takeError();
                const std::string& which = bump->str();
                if (which == "major") {
                    sv->major += 1;
                    sv->minor = 0;
                    sv->patch = 0;
                    sv->prerelease.clear();
                    sv->buildMeta.clear();
                } else if (which == "minor") {
                    sv->minor += 1;
                    sv->patch = 0;
                    sv->prerelease.clear();
                    sv->buildMeta.clear();
                } else if (which == "patch") {
                    sv->patch += 1;
                    sv->prerelease.clear();
                    sv->buildMeta.clear();
                } else {
                    return err("version: 'bump' must be major / minor "
                               "/ patch; got '" + which + "'");
                }
                newVersionStr = sv->toString();
            } else {
                return err("version: provide 'bump' (major/minor/patch) "
                           "or 'set' (<semver>)");
            }

            // Rewrite and persist.
            auto previous = rewriteVersionInPlace(src, newVersionStr);
            if (!previous) return previous.takeError();

            std::ofstream out(writeTo, std::ios::binary | std::ios::trunc);
            if (!out) return err("version: cannot write '" + writeTo + "'");
            out.write(src.data(), static_cast<std::streamsize>(src.size()));
            if (!out) return err("version: short write to '" + writeTo + "'");

            ActionResult r;
            r.outputs["version"]  = newVersionStr;
            r.outputs["previous"] = *previous;
            return r;
        }
    };

    std::unique_ptr<Action> makeVersionAction() {
        return std::make_unique<VersionAction>();
    }

} // namespace cajeta::buildtool
