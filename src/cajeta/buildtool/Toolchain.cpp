#include "cajeta/buildtool/Toolchain.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#ifndef CAJETA_VERSION
#define CAJETA_VERSION "0.0.0-unknown"
#endif

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        std::string envOr(const std::string& key,
                          const std::string& fallback) {
            const char* v = std::getenv(key.c_str());
            if (v && *v) return v;
            return fallback;
        }

        std::string trim(std::string s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                                   s.front() == '\r' || s.front() == '\n')) {
                s.erase(s.begin());
            }
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                                   s.back() == '\r' || s.back() == '\n')) {
                s.pop_back();
            }
            return s;
        }

    } // namespace

    std::string fetchPolicyToString(FetchPolicy p) {
        switch (p) {
            case FetchPolicy::Auto:  return "auto";
            case FetchPolicy::Warn:  return "warn";
            case FetchPolicy::Error: return "error";
            case FetchPolicy::Off:   return "off";
        }
        return "auto";
    }

    std::optional<FetchPolicy> fetchPolicyFromString(const std::string& s) {
        if (s == "auto")  return FetchPolicy::Auto;
        if (s == "warn")  return FetchPolicy::Warn;
        if (s == "error") return FetchPolicy::Error;
        if (s == "off")   return FetchPolicy::Off;
        return std::nullopt;
    }

    const std::vector<std::string>& reservedDistributions() {
        static const std::vector<std::string> r = {
            "official", "nightly", "lts", "system",
        };
        return r;
    }

    bool isReservedDistribution(const std::string& distribution) {
        const auto& r = reservedDistributions();
        return std::find(r.begin(), r.end(), distribution) != r.end();
    }

    llvm::Expected<std::optional<ToolchainPin>> parseToolchainPin(
        const Manifest& m) {
        const auto* settings = &m.settingsRaw;
        const auto* tc = settings->getObject("toolchain");
        if (!tc) return std::optional<ToolchainPin>{};

        // Allowed subfields. Mirrors the strict shape we use at
        // the top level — unknown keys catch typos early.
        static const std::set<std::string> allowed = {
            "version", "distribution", "channel",
            "sha256", "fetch", "from",
        };
        for (const auto& kv : *tc) {
            if (!allowed.count(kv.first.str())) {
                return err("settings.toolchain." + kv.first.str() +
                           ": unknown subfield (allowed: version, "
                           "distribution, channel, sha256, fetch, from)");
            }
        }
        ToolchainPin pin;
        if (auto v = tc->getString("version")) {
            pin.version = v->str();
        } else {
            return err("settings.toolchain: missing required 'version'");
        }
        if (auto v = tc->getString("distribution")) {
            pin.distribution = v->str();
        } else {
            pin.distribution = "official";
        }
        if (auto v = tc->getString("channel"))  pin.channel  = v->str();
        if (auto v = tc->getString("sha256"))   pin.sha256   = v->str();
        if (auto v = tc->getString("from"))     pin.fromRepo = v->str();
        if (auto v = tc->getString("fetch")) {
            auto fp = fetchPolicyFromString(v->str());
            if (!fp) {
                return err("settings.toolchain.fetch: invalid value '" +
                           v->str() +
                           "' (allowed: auto, warn, error, off)");
            }
            pin.fetch = *fp;
        }
        return std::optional<ToolchainPin>{pin};
    }

    llvm::Expected<std::optional<ToolchainPin>>
    readToolchainOverrideFile(const std::string& projectRoot) {
        std::filesystem::path p(projectRoot);
        p /= ".cajeta-toolchain";
        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) {
            return std::optional<ToolchainPin>{};
        }
        std::ifstream in(p);
        if (!in) {
            return err("cannot read '" + p.string() + "'");
        }
        std::ostringstream ss; ss << in.rdbuf();
        std::string body = trim(ss.str());
        if (body.empty()) {
            return err("'" + p.string() +
                       "' is empty — expected '<distribution>:<version>'");
        }
        auto colon = body.find(':');
        if (colon == std::string::npos) {
            return err("'" + p.string() + "': expected "
                       "'<distribution>:<version>', got '" + body + "'");
        }
        ToolchainPin pin;
        pin.distribution = body.substr(0, colon);
        pin.version      = body.substr(colon + 1);
        if (pin.distribution.empty() || pin.version.empty()) {
            return err("'" + p.string() + "': empty distribution or "
                       "version in '" + body + "'");
        }
        // Override files don't carry fetch policy — they always
        // request the default (auto).
        pin.fetch = FetchPolicy::Auto;
        return std::optional<ToolchainPin>{pin};
    }

    llvm::Expected<ResolvedToolchain> resolveEffectiveToolchain(
        const Manifest* manifest,
        const std::string& projectRoot) {
        ResolvedToolchain out;
        // 1. .cajeta-toolchain override (highest precedence).
        auto override_ = readToolchainOverrideFile(projectRoot);
        if (!override_) return override_.takeError();
        if (override_->has_value()) {
            out.hasPin = true;
            out.pin = **override_;
            out.sourcePath = (std::filesystem::path(projectRoot) /
                              ".cajeta-toolchain").string();
            return out;
        }
        // 2. settings.toolchain manifest block.
        if (manifest) {
            auto pin = parseToolchainPin(*manifest);
            if (!pin) return pin.takeError();
            if (pin->has_value()) {
                out.hasPin = true;
                out.pin = **pin;
                out.sourcePath = manifest->sourcePath;
                return out;
            }
        }
        // 3. No pin — caller dispatches to the running binary.
        return out;
    }

    std::string ToolchainStoreLayout::binaryPath(
        const std::string& distribution,
        const std::string& version) const {
        return (std::filesystem::path(root) /
                distribution / version / "bin" / "cajeta").string();
    }

    std::string ToolchainStoreLayout::installRoot(
        const std::string& distribution,
        const std::string& version) const {
        return (std::filesystem::path(root) /
                distribution / version).string();
    }

    std::string ToolchainStoreLayout::defaultSymlinkPath() const {
        return (std::filesystem::path(root) / "current").string();
    }

    ToolchainStoreLayout resolveToolchainStoreLayout(
        const std::string& homeOverride) {
        ToolchainStoreLayout layout;
        std::string home = !homeOverride.empty()
                              ? homeOverride
                              : envOr("CAJETA_TOOLCHAIN_HOME", "");
        if (!home.empty()) {
            layout.root = home;
        } else {
            std::string h = envOr("HOME", "");
#ifdef _WIN32
            if (h.empty()) h = envOr("USERPROFILE", "C:\\");
#else
            if (h.empty()) h = "/tmp";
#endif
            layout.root = (std::filesystem::path(h) /
                           ".cajeta" / "toolchains").string();
        }
        return layout;
    }

    std::vector<InstalledToolchain> listInstalledToolchains(
        const ToolchainStoreLayout& layout) {
        std::vector<InstalledToolchain> out;
        std::error_code ec;
        if (!std::filesystem::exists(layout.root, ec)) return out;

        // Resolve the `current` symlink (if any) so we can mark
        // the default toolchain.
        std::string currentTarget;
        std::error_code lec;
        auto symlink = std::filesystem::path(layout.defaultSymlinkPath());
        if (std::filesystem::is_symlink(symlink, lec)) {
            auto target = std::filesystem::read_symlink(symlink, lec);
            if (!lec) currentTarget = target.string();
        }

        for (auto& distEntry : std::filesystem::directory_iterator(
                                   layout.root, ec)) {
            if (!distEntry.is_directory()) continue;
            auto distName = distEntry.path().filename().string();
            if (distName == "current") continue;
            for (auto& verEntry : std::filesystem::directory_iterator(
                                     distEntry.path(), ec)) {
                if (!verEntry.is_directory()) continue;
                InstalledToolchain it;
                it.distribution = distName;
                it.version      = verEntry.path().filename().string();
                it.installRoot  = verEntry.path().string();
                // Match the current-symlink target (we accept both
                // absolute paths and `<dist>/<ver>` relative forms).
                if (!currentTarget.empty()) {
                    auto rel = (std::filesystem::path(distName) /
                                it.version).string();
                    if (currentTarget == it.installRoot ||
                        currentTarget == rel) {
                        it.isDefault = true;
                    }
                }
                out.push_back(std::move(it));
            }
        }
        std::sort(out.begin(), out.end(),
            [](const InstalledToolchain& a,
               const InstalledToolchain& b) {
                if (a.distribution != b.distribution) {
                    return a.distribution < b.distribution;
                }
                return a.version > b.version;
            });
        return out;
    }

    bool runningBinarySatisfiesPin(const ResolvedToolchain& tc,
                                    const std::string& runningVersion) {
        if (!tc.hasPin) return true;
        std::string running = runningVersion.empty()
                                  ? std::string(CAJETA_VERSION)
                                  : runningVersion;
        return running == tc.pin.version;
    }

    llvm::Expected<DispatchDecision> computeDispatchDecision(
        const ResolvedToolchain& tc,
        const ToolchainStoreLayout& layout,
        const std::string& runningVersion) {
        DispatchDecision d;
        const char* dispatchOff = std::getenv("CAJETA_NO_DISPATCH");
        if (dispatchOff && *dispatchOff) {
            d.action = DispatchAction::Continue;
            d.notes.push_back("CAJETA_NO_DISPATCH set — running PATH "
                              "binary regardless of pin");
            return d;
        }
        if (!tc.hasPin) {
            d.action = DispatchAction::Continue;
            return d;
        }
        if (tc.pin.fetch == FetchPolicy::Off) {
            d.action = DispatchAction::Continue;
            d.notes.push_back("toolchain pin present but fetch=off — "
                              "running PATH binary");
            return d;
        }
        std::string running = runningVersion.empty()
                                  ? std::string(CAJETA_VERSION)
                                  : runningVersion;
        if (running == tc.pin.version) {
            d.action = DispatchAction::Continue;
            return d;
        }
        std::string pinned = layout.binaryPath(
            tc.pin.distribution, tc.pin.version);
        std::error_code ec;
        if (std::filesystem::exists(pinned, ec)) {
            d.action = DispatchAction::ReExec;
            d.resolvedBinaryPath = pinned;
            return d;
        }
        // Pinned binary not installed.
        std::string installCmd = "cajeta toolchain install " +
                                  tc.pin.distribution + ":" +
                                  tc.pin.version;
        switch (tc.pin.fetch) {
            case FetchPolicy::Auto:
                d.action = DispatchAction::NeedsInstall;
                d.installHint = installCmd;
                return d;
            case FetchPolicy::Warn:
                d.action = DispatchAction::Continue;
                d.notes.push_back(
                    "toolchain pin " + tc.pin.distribution + ":" +
                    tc.pin.version + " not installed — running the "
                    "PATH binary (fetch=warn). Install with: " +
                    installCmd);
                return d;
            case FetchPolicy::Error:
                return err("toolchain pin " + tc.pin.distribution +
                           ":" + tc.pin.version + " not installed "
                           "and fetch=error — install with: " +
                           installCmd);
            case FetchPolicy::Off:
                d.action = DispatchAction::Continue;
                return d;
        }
        d.action = DispatchAction::Continue;
        return d;
    }

    std::string toolchainIdentity(const ResolvedToolchain& tc,
                                   const std::string& runningVersion) {
        if (!tc.hasPin) {
            std::string running = runningVersion.empty()
                                      ? std::string(CAJETA_VERSION)
                                      : runningVersion;
            return "official:" + running;
        }
        std::string out = tc.pin.distribution + ":" + tc.pin.version;
        if (tc.pin.sha256.has_value() && !tc.pin.sha256->empty()) {
            out += ":" + *tc.pin.sha256;
        }
        return out;
    }

} // namespace cajeta::buildtool
