#include "cajeta/buildtool/Manifest.h"

#include "cajeta/buildtool/Flavor.h"
#include "cajeta/buildtool/JsonC.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

#include <filesystem>
#include <set>
#include <string>

namespace cajeta::buildtool {

    namespace {

        // The valid top-level blocks in a manifest. Six core blocks
        // plus two specialty blocks (workspace + melt) that are
        // mutually exclusive with `tasks`/source content. See
        // BuildTool.md "Manifest — cajeta.json". Phase 6c adds the
        // typed parsers for `workspace` + `melt`; today they parse
        // through as raw objects on the manifest.
        const std::set<std::string> kTopLevelBlocks = {
            "details", "properties", "settings",
            "actions", "plugins", "tasks",
            "workspace", "melt",
        };

        // Fields recognized inside the `details` block. Phase 0 is
        // strict here: an unknown field is a load error, because the
        // identity block's schema should be stable. Other blocks
        // tolerate unknown fields during Phase 0 (modeled raw).
        const std::set<std::string> kDetailsFields = {
            "name", "version", "description", "license",
            "authors", "repository-url", "cajeta-lang-version",
            // `plugin` carries the plugin-specific sub-block
            // (id / actions / entries / binary) for packages that
            // ship as plugins. Present only on plugin sidecars;
            // captured raw because each runtime version owns the
            // shape of that block.
            "plugin",
        };

        // Build a citation-style error. `where` is a human-readable
        // location prefix (file/source label); `msg` is the actual
        // explanation. Returned as an llvm::Error suitable for
        // propagation through Expected<>.
        llvm::Error cite(const std::string& where, const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(),
                where + ": " + msg);
        }

        // Type predicates: the spec rejects null where a value is
        // expected, and requires the block-typed values to actually be
        // objects (not arrays, strings, etc.).
        llvm::Error requireString(const std::string& where,
                                  const std::string& field,
                                  const llvm::json::Value* v,
                                  std::string& out) {
            if (!v) {
                return cite(where, "missing required field '" + field + "'");
            }
            auto s = v->getAsString();
            if (!s) {
                return cite(where, "field '" + field + "' must be a string");
            }
            out = s->str();
            return llvm::Error::success();
        }

        llvm::Error requireObject(const std::string& where,
                                  const std::string& block,
                                  const llvm::json::Value* v,
                                  llvm::json::Object& out) {
            if (!v) {
                // Missing optional blocks are allowed; caller decides
                // whether to call this. When the block is missing
                // entirely we leave `out` as the empty default.
                return llvm::Error::success();
            }
            const auto* o = v->getAsObject();
            if (!o) {
                return cite(where, "'" + block + "' must be an object");
            }
            out = *o;
            return llvm::Error::success();
        }

        // Load the `details` block. Required block; the manifest is
        // ill-formed without it.
        llvm::Error loadDetails(const std::string& where,
                                const llvm::json::Object& root,
                                ManifestDetails& out) {
            const auto* detailsV = root.get("details");
            if (!detailsV) {
                return cite(where, "missing required top-level block 'details'");
            }
            const auto* d = detailsV->getAsObject();
            if (!d) {
                return cite(where, "'details' must be an object");
            }

            // Reject unknown details fields — keeps the identity schema
            // pinned.
            for (const auto& kv : *d) {
                if (!kDetailsFields.count(kv.first.str())) {
                    return cite(where,
                        "unknown field in 'details': '" + kv.first.str() +
                        "' (allowed: name, version, description, license, "
                        "authors, repository-url, cajeta-lang-version)");
                }
            }

            if (auto e = requireString(
                    where + ".details", "name", d->get("name"), out.name)) {
                return e;
            }
            if (auto e = requireString(
                    where + ".details", "version", d->get("version"), out.version)) {
                return e;
            }

            // Optional string fields.
            auto getOptionalString = [&](const char* field,
                                         std::optional<std::string>& dst) -> llvm::Error {
                const auto* v = d->get(field);
                if (!v) return llvm::Error::success();
                auto s = v->getAsString();
                if (!s) {
                    return cite(where + ".details",
                        std::string("field '") + field + "' must be a string");
                }
                dst = s->str();
                return llvm::Error::success();
            };
            if (auto e = getOptionalString("description", out.description)) return e;
            if (auto e = getOptionalString("license", out.license)) return e;
            if (auto e = getOptionalString("repository-url", out.repositoryUrl)) return e;
            if (auto e = getOptionalString("cajeta-lang-version", out.cajetaLangVersion)) return e;

            // Authors array — optional, each entry must be a string.
            if (const auto* a = d->get("authors")) {
                const auto* arr = a->getAsArray();
                if (!arr) {
                    return cite(where + ".details", "'authors' must be an array");
                }
                for (size_t i = 0; i < arr->size(); ++i) {
                    auto s = (*arr)[i].getAsString();
                    if (!s) {
                        return cite(where + ".details",
                            "'authors[" + std::to_string(i) + "]' must be a string");
                    }
                    out.authors.push_back(s->str());
                }
            }

            // Plugin sub-block — optional. Captured raw because the
            // shape is owned by the plugin protocol, not the
            // manifest schema (Plugin.cpp's resolvePlugins reads it).
            if (const auto* p = d->get("plugin")) {
                const auto* obj = p->getAsObject();
                if (!obj) {
                    return cite(where + ".details",
                        "'plugin' must be an object");
                }
                out.pluginRaw = *obj;
            }

            return llvm::Error::success();
        }

    } // namespace

    std::string ManifestDetails::group() const {
        auto dot = name.rfind('.');
        if (dot == std::string::npos) return "";
        return name.substr(0, dot);
    }

    std::string ManifestDetails::library() const {
        auto dot = name.rfind('.');
        if (dot == std::string::npos) return name;
        return name.substr(dot + 1);
    }

    llvm::Expected<Manifest> loadManifestString(const std::string& source,
                                                const std::string& sourceLabel) {
        auto val = parseJsonC(source);
        if (!val) {
            std::string msg;
            llvm::raw_string_ostream os(msg);
            os << "in '" << sourceLabel << "': " << val.takeError();
            return llvm::createStringError(llvm::inconvertibleErrorCode(), msg);
        }

        const auto* root = val->getAsObject();
        if (!root) {
            return cite(sourceLabel, "manifest root must be a JSON object");
        }

        // Reject unknown top-level blocks. This catches typos
        // (e.g. "settigns" for "settings") early rather than letting
        // the field silently disappear.
        for (const auto& kv : *root) {
            if (!kTopLevelBlocks.count(kv.first.str())) {
                return cite(sourceLabel,
                    "unknown top-level block '" + kv.first.str() +
                    "' (allowed: details, properties, settings, actions, "
                    "plugins, tasks, workspace, melt)");
            }
        }

        Manifest m;
        m.sourcePath = sourceLabel;

        if (auto e = loadDetails(sourceLabel, *root, m.details)) {
            return std::move(e);
        }

        // Other blocks: store raw and let later phases type them.
        // Validate they're objects when present.
        if (auto e = requireObject(
                sourceLabel, "properties",
                root->get("properties"), m.propertiesRaw)) return std::move(e);
        if (auto e = requireObject(
                sourceLabel, "settings",
                root->get("settings"), m.settingsRaw)) return std::move(e);
        if (auto e = requireObject(
                sourceLabel, "actions",
                root->get("actions"), m.actionsRaw)) return std::move(e);
        if (auto e = requireObject(
                sourceLabel, "plugins",
                root->get("plugins"), m.pluginsRaw)) return std::move(e);
        if (auto e = requireObject(
                sourceLabel, "tasks",
                root->get("tasks"), m.tasksRaw)) return std::move(e);
        if (root->get("melt")) {
            m.hasMelt = true;
            if (auto e = requireObject(
                    sourceLabel, "melt",
                    root->get("melt"), m.meltRaw)) return std::move(e);
        }
        if (root->get("workspace")) {
            m.hasWorkspace = true;
            if (auto e = requireObject(
                    sourceLabel, "workspace",
                    root->get("workspace"), m.workspaceRaw)) return std::move(e);
        }

        // Phase 8: custom-flavor validation at load time. Walks every
        // settings.build.custom-flavors entry — unknown property keys
        // and base-chain cycles surface here rather than mid-build.
        if (const auto* settings = root->getObject("settings")) {
            if (const auto* build = settings->getObject("build")) {
                if (const auto* cf = build->getObject("custom-flavors")) {
                    if (auto e = validateCustomFlavors(*cf)) {
                        // Prepend the source label so the error
                        // points at the manifest, not just the
                        // offending custom-flavor entry.
                        std::string msg;
                        llvm::raw_string_ostream os(msg);
                        os << e;
                        consumeError(std::move(e));
                        return cite(sourceLabel, msg);
                    }
                }
            }
        }

        // Melt mutual exclusion. A melt package's purpose is to export
        // curated configuration — it has no source, no tasks, and
        // can't simultaneously be a workspace root. Per BuildTool.md
        // "Anatomy of a melt package" + Phase 6c plan: reject these
        // combinations early so consumers can't accidentally publish
        // a hybrid.
        if (m.hasMelt) {
            if (!m.tasksRaw.empty()) {
                return cite(sourceLabel,
                    "manifest declares both 'melt' and 'tasks' — a "
                    "melt package exports configuration only and "
                    "cannot define tasks");
            }
            if (m.hasWorkspace) {
                return cite(sourceLabel,
                    "manifest declares both 'melt' and 'workspace' "
                    "— these are mutually exclusive");
            }
        }

        return m;
    }

    llvm::Expected<SettingsOutput> parseSettingsOutput(const Manifest& m) {
        SettingsOutput out;
        const auto* output = m.settingsRaw.getObject("output");
        if (!output) return out;   // absent → defaults; valid

        // One validator for all four keys, so they cannot drift apart.
        // ABSOLUTE PATHS ARE LEGAL (§3.3 says values are relative to the
        // project root "unless absolute", and §3.3.1 wants intermediates
        // redirected to tmpfs) — what is rejected is a value that is not a
        // string, is empty, or ESCAPES the project root. An escape is the
        // real hazard: `../..` would put generated files somewhere the
        // project does not own and `clean` would never find them again.
        auto read = [&](const char* key,
                        std::optional<std::string>& slot)
                -> llvm::Error {
            const auto* v = output->get(key);
            if (!v) return llvm::Error::success();
            auto s = v->getAsString();
            if (!s) {
                return llvm::createStringError(
                    llvm::inconvertibleErrorCode(),
                    std::string("settings.output.") + key +
                    ": value must be a string path");
            }
            if (s->empty()) {
                return llvm::createStringError(
                    llvm::inconvertibleErrorCode(),
                    std::string("settings.output.") + key +
                    ": value must not be empty");
            }
            std::filesystem::path p(s->str());
            if (p.is_relative()) {
                std::filesystem::path norm = p.lexically_normal();
                auto it = norm.begin();
                if (it != norm.end() && it->string() == "..") {
                    return llvm::createStringError(
                        llvm::inconvertibleErrorCode(),
                        std::string("settings.output.") + key + ": '"
                        + s->str() + "' escapes the project root; generated"
                        " files must stay inside it (an absolute path is"
                        " allowed if you mean somewhere else entirely)");
                }
            }
            slot = s->str();
            return llvm::Error::success();
        };

        if (auto e = read("root", out.root)) return std::move(e);
        if (auto e = read("intermediates", out.intermediates))
            return std::move(e);
        if (auto e = read("artifacts", out.artifacts)) return std::move(e);
        if (auto e = read("binaries", out.binaries)) return std::move(e);
        return out;
    }

    llvm::Expected<SettingsBuild> parseSettingsBuild(const Manifest& m) {
        auto err = [](const std::string& where, const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(),
                where + ": " + msg);
        };

        SettingsBuild out;
        const auto* settings = &m.settingsRaw;
        const auto* build = settings->getObject("build");
        if (!build) {
            return out;  // no settings.build → defaults; valid
        }
        if (auto v = build->getString("entry-method"))
            out.entryMethod = v->str();
        if (auto v = build->getString("target"))
            out.target = v->str();
        if (auto v = build->getString("source-root"))
            out.sourceRoot = v->str();
        if (auto v = build->getString("output-dir"))
            out.outputDir = v->str();
        if (const auto* cf = build->getObject("custom-flavors")) {
            out.customFlavorsRaw = *cf;
        }
        if (const auto* cache = build->getObject("cache")) {
            if (auto v = cache->getInteger("max-bytes"); v && *v > 0)
                out.cacheMaxBytes = static_cast<uint64_t>(*v);
            if (auto v = cache->getInteger("max-age-seconds"); v && *v > 0)
                out.cacheMaxAgeSeconds = static_cast<uint64_t>(*v);
        }
        if (const auto* binaries = build->getObject("binaries")) {
            for (const auto& kv : *binaries) {
                BinarySpec b;
                b.name = kv.first.str();
                const auto* spec = kv.second.getAsObject();
                if (!spec) {
                    return err("settings.build.binaries",
                               "entry '" + b.name + "' must be an object");
                }
                auto em = spec->getString("entry-method");
                if (!em) {
                    return err("settings.build.binaries." + b.name,
                               "missing required 'entry-method'");
                }
                b.entryMethod = em->str();
                if (auto d = spec->getString("description"))
                    b.description = d->str();
                out.binaries[b.name] = std::move(b);
            }
        }
        return out;
    }

    llvm::Expected<NativeLibrary> parseNativeLibraryEntry(
            const std::string& id, const llvm::json::Object& o,
            const std::string& where) {
        auto err = [&](const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), where + ": " + msg);
        };
        NativeLibrary lib;
        lib.id = id;
        auto ver = o.getString("version");
        if (!ver) return err("missing required 'version'");
        lib.version = ver->str();
        auto lic = o.getString("license");
        if (!lic) return err("missing required 'license'");
        lib.license = lic->str();
        if (auto r = o.getBoolean("redistributable"))
            lib.redistributable = *r;
        if (auto lk = o.getString("link")) {
            lib.link = lk->str();
            if (lib.link != "static" && lib.link != "dynamic")
                return err("unknown link mode '" + lib.link +
                           "' (expected 'static' or 'dynamic')");
        }
        if (const auto* plats = o.getArray("platforms")) {
            for (const auto& p : *plats) {
                if (auto ps = p.getAsString())
                    lib.platforms.push_back(ps->str());
                else
                    return err("platforms entries must be strings");
            }
        }
        if (const auto* arts = o.getObject("artifacts")) {
            for (const auto& akv : *arts) {
                NativeArtifact a;
                a.platform = akv.first.str();
                const auto* ao = akv.second.getAsObject();
                if (!ao)
                    return err("artifacts." + a.platform + " must be an object");
                if (auto u = ao->getString("url")) a.url = u->str();
                if (auto s = ao->getString("sha256")) a.sha256 = s->str();
                lib.artifacts[a.platform] = std::move(a);
            }
        }
        if (auto acq = o.getString("acquire")) lib.acquire = acq->str();
        return lib;
    }

    llvm::Expected<std::map<std::string, NativeLibrary>>
    parseNativeLibraries(const Manifest& m) {
        std::map<std::string, NativeLibrary> out;
        const auto* nl = m.settingsRaw.getObject("native-libraries");
        if (!nl) return out;  // no block → empty; valid
        for (const auto& kv : *nl) {
            const std::string id = kv.first.str();
            const std::string where = "settings.native-libraries." + id;
            const auto* o = kv.second.getAsObject();
            if (!o)
                return llvm::createStringError(
                    llvm::inconvertibleErrorCode(),
                    where + ": entry must be an object");
            auto lib = parseNativeLibraryEntry(id, *o, where);
            if (!lib) return lib.takeError();
            out[id] = std::move(*lib);
        }
        return out;
    }

    llvm::Expected<std::map<std::string, NativeOverride>>
    parseNativeOverrides(const Manifest& m) {
        auto err = [](const std::string& where, const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), where + ": " + msg);
        };
        std::map<std::string, NativeOverride> out;
        const auto* no = m.settingsRaw.getObject("native-overrides");
        if (!no) return out;
        for (const auto& kv : *no) {
            NativeOverride ov;
            const std::string id = kv.first.str();
            const std::string where = "settings.native-overrides." + id;
            const auto* o = kv.second.getAsObject();
            if (!o) return err(where, "entry must be an object");
            if (auto v = o->getString("version")) ov.version = v->str();
            if (auto p = o->getString("path")) ov.path = p->str();
            if (!ov.version && !ov.path)
                return err(where, "must set 'version' or 'path'");
            out[id] = std::move(ov);
        }
        return out;
    }

    llvm::Expected<Manifest> loadManifestFile(const std::string& path) {
        auto buf = llvm::MemoryBuffer::getFile(path);
        if (!buf) {
            return llvm::createStringError(
                buf.getError(),
                "cannot open manifest file '" + path + "': " +
                buf.getError().message());
        }
        return loadManifestString((*buf)->getBuffer().str(), path);
    }

    std::string projectRootFromManifest(const Manifest& m) {
        if (m.sourcePath.empty()) return ".";
        auto parent = std::filesystem::path(m.sourcePath).parent_path();
        return parent.empty() ? "." : parent.string();
    }

} // namespace cajeta::buildtool
