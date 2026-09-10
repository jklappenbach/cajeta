// The `version` action: bump or set the manifest's semver, writing the manifest
// back in place and reporting the new and previous values.

#include "cajeta/buildtool/Action.h"

#include <llvm/Support/Error.h>

#include <cctype>
#include <fstream>
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

        // Parse MAJOR.MINOR.PATCH[-prerelease][+build]. Hand-scanned rather than
        // by std::regex, whose libstdc++ COMDAT symbols collide at link time with
        // the prebuilt LLVM/lld archives on the mingw toolchain.
        llvm::Expected<Semver> parseSemver(const std::string& s) {
            auto bad = [&]() {
                return err("version: not a valid semver: '" + s + "'");
            };
            const size_t n = s.size();
            size_t i = 0;
            auto readNum = [&](unsigned& out) -> bool {
                size_t start = i;
                while (i < n && s[i] >= '0' && s[i] <= '9') ++i;
                if (i == start) return false;
                try {
                    out = static_cast<unsigned>(std::stoul(s.substr(start, i - start)));
                } catch (...) {
                    return false;
                }
                return true;
            };
            auto isIdent = [](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) ||
                       c == '.' || c == '-';
            };
            auto readIdent = [&](std::string& out) -> bool {
                size_t start = i;
                while (i < n && isIdent(s[i])) ++i;
                if (i == start) return false;  // empty run — invalid
                out = s.substr(start, i - start);
                return true;
            };

            Semver v;
            if (!readNum(v.major)) return bad();
            if (i >= n || s[i] != '.') return bad();
            ++i;
            if (!readNum(v.minor)) return bad();
            if (i >= n || s[i] != '.') return bad();
            ++i;
            if (!readNum(v.patch)) return bad();
            if (i < n && s[i] == '-') {
                ++i;
                if (!readIdent(v.prerelease)) return bad();
            }
            if (i < n && s[i] == '+') {
                ++i;
                if (!readIdent(v.buildMeta)) return bad();
            }
            if (i != n) return bad();
            return v;
        }

        // Replace `details.version` in a JSONC source with `newVersion`, returning
        // the previous value and leaving every other byte — comments, formatting,
        // trailing commas — untouched. Best-effort: the FIRST such key wins.
        llvm::Expected<std::string> rewriteVersionInPlace(
            std::string& src,
            const std::string& newVersion) {
            auto detailsPos = src.find("\"details\"");
            if (detailsPos == std::string::npos) {
                return err("version: manifest has no \"details\" block");
            }
            auto isWs = [](char c) {
                return std::isspace(static_cast<unsigned char>(c)) != 0;
            };
            size_t scan = detailsPos;
            for (;;) {
                size_t vpos = src.find("\"version\"", scan);
                if (vpos == std::string::npos) {
                    return err("version: no version field found after "
                               "\"details\" in the manifest");
                }
                size_t j = vpos + 9;  // past the literal "version"
                while (j < src.size() && isWs(src[j])) ++j;
                if (j < src.size() && src[j] == ':') {
                    ++j;
                    while (j < src.size() && isWs(src[j])) ++j;
                    if (j < src.size() && src[j] == '"') {
                        size_t valStart = j + 1;
                        size_t valEnd = src.find('"', valStart);
                        if (valEnd != std::string::npos) {
                            std::string previous =
                                src.substr(valStart, valEnd - valStart);
                            src.replace(valStart, valEnd - valStart, newVersion);
                            return previous;
                        }
                    }
                }
                scan = vpos + 9;  // this key didn't fit the shape; keep scanning
            }
        }

    } // namespace

    class VersionAction : public Action {
    public:
        std::string name() const override { return "version"; }

        // Takes exactly one of `bump` ("major"/"minor"/"patch") or `set`, plus an
        // optional `write-to`; reports back `version` and `previous`.
        llvm::Expected<ActionResult> run(
            const llvm::json::Object& params,
            TaskContext& /*ctx*/) const override {

            std::string writeTo = "./cajeta.json";
            if (auto v = params.getString("write-to")) writeTo = v->str();

            std::string src;
            {
                std::ifstream in(writeTo);
                if (!in) return err("version: cannot read manifest '" +
                                    writeTo + "'");
                std::ostringstream ss; ss << in.rdbuf();
                src = ss.str();
            }

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
                // A throwaway rewrite is how the current version is read back;
                // the real rewrite happens below.
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
