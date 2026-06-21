#include "cajeta/mcp/McpServer.h"

#include "cajeta/buildtool/ArtifactCache.h"
#include "cajeta/buildtool/Lockfile.h"
#include "cajeta/buildtool/Subprocess.h"
#include "cajeta/buildtool/repo/TarZstd.h"
#include "cajeta/buildtool/skill/SkillCli.h"

#include <llvm/Support/Base64.h>
#include <llvm/Support/Error.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace cajeta::mcp {

    namespace fs = std::filesystem;

    namespace {

        // MCP protocol revision this server implements.
        constexpr const char* kProtocolVersion = "2024-11-05";
        constexpr const char* kServerName = "cajeta";
        constexpr const char* kServerVersion = "0.1.0";

        // Resolve this process's own executable path (the cajeta binary in
        // production). Linux: /proc/self/exe; fallback to argv0.
        std::string resolveSelfExe(const char* argv0) {
            std::error_code ec;
            fs::path p = fs::read_symlink("/proc/self/exe", ec);
            if (!ec && !p.empty()) return p.string();
            return argv0 ? std::string(argv0) : std::string("cajeta");
        }

        // Make a fresh unique temp directory under the system temp dir.
        fs::path makeTempDir(const std::string& tag) {
            static std::atomic<unsigned> counter{0};
            fs::path base = fs::temp_directory_path();
            fs::path dir = base / ("cajeta_mcp_" + tag + "_"
                + std::to_string((long long) ::getpid()) + "_"
                + std::to_string(counter.fetch_add(1)));
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir;
        }

        // Decode a base64 tar.zstd archive into its entries. Returns false with
        // *err set on a malformed archive.
        bool decodeArchive(const std::string& b64,
                           std::vector<buildtool::TarEntry>* out,
                           std::string* err) {
            std::vector<char> raw;
            if (llvm::Error e = llvm::decodeBase64(b64, raw)) {
                *err = "base64: " + llvm::toString(std::move(e));
                return false;
            }
            auto entries = buildtool::readTarZstd(std::string(raw.begin(), raw.end()));
            if (!entries) {
                *err = "tar.zstd: " + llvm::toString(entries.takeError());
                return false;
            }
            *out = std::move(*entries);
            return true;
        }

        // Write archive entries under `root`, creating parent dirs.
        void writeEntries(const fs::path& root,
                          const std::vector<buildtool::TarEntry>& entries) {
            for (const auto& e : entries) {
                fs::path target = root / e.name;
                std::error_code ec;
                fs::create_directories(target.parent_path(), ec);
                std::ofstream f(target, std::ios::binary);
                f.write(e.data.data(), (std::streamsize) e.data.size());
            }
        }

        // Split captured stderr into one diagnostic string per non-empty line.
        Json diagnosticsFrom(const std::string& stderrText) {
            Json arr = Json::array();
            std::istringstream ss(stderrText);
            std::string line;
            while (std::getline(ss, line)) {
                if (!line.empty()) arr.push_back(line);
            }
            return arr;
        }

        const char* tierToString(buildtool::skill::MatchTier t) {
            using buildtool::skill::MatchTier;
            switch (t) {
                case MatchTier::Exact:            return "exact";
                case MatchTier::Descendant:       return "descendant";
                case MatchTier::AncestorOverview: return "ancestorOverview";
            }
            return "unknown";
        }

        // Optional string param: present + string → its value, else nullopt.
        std::optional<std::string> optStr(const Json& args, const std::string& k) {
            if (args.isObject() && args.has(k) && args.at(k).isString()) {
                return args.at(k).asString();
            }
            return std::nullopt;
        }

        Json makeResult(const Json& id, Json result) {
            Json r = Json::object();
            r["jsonrpc"] = "2.0";
            r["id"] = id;                  // echoed; Null for parse errors
            r["result"] = std::move(result);
            return r;
        }

        Json makeError(const Json& id, int code, const std::string& message) {
            Json r = Json::object();
            r["jsonrpc"] = "2.0";
            r["id"] = id;
            Json e = Json::object();
            e["code"] = code;
            e["message"] = message;
            r["error"] = std::move(e);
            return r;
        }

        // A `{content:[{type:"text", text:...}]}` tools/call result body.
        Json textContent(const std::string& text, bool isError = false) {
            Json item = Json::object();
            item["type"] = "text";
            item["text"] = text;
            Json arr = Json::array();
            arr.push_back(std::move(item));
            Json body = Json::object();
            body["content"] = std::move(arr);
            if (isError) body["isError"] = true;
            return body;
        }

    } // namespace

    // Default skill backend: load <projectRoot_>/cajeta.lock + the ArtifactCache
    // and call the real cores. Lambdas capture `this` so they read projectRoot_
    // live (setProjectRoot takes effect without rebinding). Throws McpError when
    // the project context is missing.
    void McpServer::installDefaultSkillBackend() {
        namespace bt = cajeta::buildtool;
        namespace sk = cajeta::buildtool::skill;
        auto loadContext = [this]() -> sk::SkillSearchContext {
            auto lf = bt::readLockfile(projectRoot_ + "/cajeta.lock");
            if (!lf) {
                llvm::consumeError(lf.takeError());
                throw McpError{-32603, "cajeta.lock not found under " + projectRoot_};
            }
            bt::ArtifactCache cache(projectRoot_);
            auto ctx = sk::loadSkillSearchContext(
                lf->packagesTyped,
                [&](llvm::StringRef s) { return cache.lookup(s.str()); });
            if (!ctx) {
                llvm::consumeError(ctx.takeError());
                throw McpError{-32603, "failed to load skill context"};
            }
            return std::move(*ctx);
        };
        skill_.search = [this, loadContext](const std::string& name,
                                 std::optional<std::string> version,
                                 std::optional<std::string> from,
                                 sk::MatchOptions opts) {
            return sk::searchSkills(name, version, from, loadContext(), opts);
        };
        skill_.list = [loadContext](std::optional<std::string> scope,
                               std::optional<std::string> version,
                               std::optional<std::string> from) {
            return sk::listSkills(scope, version, from, loadContext());
        };
        skill_.get = [this](const std::vector<std::string>& uris) {
            auto lf = bt::readLockfile(projectRoot_ + "/cajeta.lock");
            if (!lf) {
                llvm::consumeError(lf.takeError());
                throw McpError{-32603, "cajeta.lock not found under " + projectRoot_};
            }
            bt::ArtifactCache cache(projectRoot_);
            return sk::getSkills(uris, lf->packagesTyped,
                [&](llvm::StringRef s) { return cache.lookup(s.str()); });
        };
    }

    McpServer::McpServer() {
        compilerExePath_ = resolveSelfExe(nullptr);
        installDefaultSkillBackend();
        registerSkillTools();
        registerCompileTool();

        // jit_execute remains a stub until its unit lands; registering it here
        // keeps tools/list complete.
        Json objectSchema = Json::object();
        objectSchema["type"] = "object";
        registerTool("jit_execute",
            "Compile and JIT-execute a source archive (process-per-execute).",
            objectSchema, [](const Json&) -> Json {
                return textContent("not implemented", /*isError=*/true);
            });
    }

    void McpServer::registerCompileTool() {
        Json schema = Json::object();
        schema["type"] = "object";
        {
            Json props = Json::object();
            Json archiveProp = Json::object();
            archiveProp["type"] = "string";
            archiveProp["description"] = "base64-encoded tar.zstd source tree";
            props["archive"] = archiveProp;
            schema["properties"] = props;
            Json req = Json::array();
            req.push_back(std::string("archive"));
            schema["required"] = req;
        }
        registerTool("compile",
            "Compile a base64 tar.zstd source archive; return diagnostics + artifact.",
            schema, [this](const Json& args) -> Json {
                if (!args.isObject() || !args.has("archive")
                        || !args.at("archive").isString()) {
                    throw McpError{-32602, "compile requires base64 'archive'"};
                }
                std::vector<buildtool::TarEntry> entries;
                std::string err;
                if (!decodeArchive(args.at("archive").asString(), &entries, &err)) {
                    throw McpError{-32602, "invalid archive: " + err};
                }
                if (entries.empty()) {
                    throw McpError{-32602, "archive is empty"};
                }
                const std::string emit = optStr(args, "emit").value_or("cja");
                const std::string entry = optStr(args, "entryMethod").value_or("");

                fs::path src = makeTempDir("compile_src");
                fs::path out = makeTempDir("compile_out");
                writeEntries(src, entries);

                std::string outData, errData;
                buildtool::SubprocessOptions opt;
                opt.argv = {compilerExePath_, "--emit=" + emit, entry,
                            src.string(), out.string()};
                opt.outData = &outData;
                opt.errData = &errData;
                buildtool::SubprocessResult res = runSubprocess(opt);

                // Collect the emitted .cja (if any) as a base64 artifact.
                std::optional<std::string> artifact;
                if (res.launched && res.code() == 0) {
                    std::error_code ec;
                    for (auto& de : fs::directory_iterator(out, ec)) {
                        if (de.path().extension() == ".cja") {
                            std::ifstream f(de.path(), std::ios::binary);
                            std::string bytes((std::istreambuf_iterator<char>(f)),
                                              std::istreambuf_iterator<char>());
                            artifact = llvm::encodeBase64(bytes);
                            break;
                        }
                    }
                }

                std::error_code ec;
                fs::remove_all(src, ec);
                fs::remove_all(out, ec);

                if (!res.launched) {
                    throw McpError{-32603, "failed to launch compiler: " + res.error};
                }
                const int status = res.code();
                Json body = textContent(
                    status == 0 ? "compiled" : errData,
                    /*isError=*/status != 0);
                body["exitStatus"] = status;
                body["diagnostics"] = diagnosticsFrom(errData);
                if (artifact) body["artifact"] = *artifact;
                return body;
            });
    }

    void McpServer::setSkillBackend(SkillBackend backend) {
        skill_ = std::move(backend);
        registerSkillTools();
    }

    void McpServer::registerSkillTools() {
        Json nameSchema = Json::object();
        nameSchema["type"] = "object";
        {
            Json props = Json::object();
            Json nameProp = Json::object();
            nameProp["type"] = "string";
            props["name"] = nameProp;
            nameSchema["properties"] = props;
            Json req = Json::array();
            req.push_back(std::string("name"));
            nameSchema["required"] = req;
        }

        registerTool("searchSkills",
            "Search skills by canonical name (fuzzy, typo-tolerant).",
            nameSchema, [this](const Json& args) -> Json {
                if (!args.isObject() || !args.has("name")
                        || !args.at("name").isString()) {
                    throw McpError{-32602, "searchSkills requires string 'name'"};
                }
                buildtool::skill::MatchOptions opts;
                opts.exact = args.has("exact") && args.at("exact").asBool();
                auto results = skill_.search(args.at("name").asString(),
                    optStr(args, "version"), optStr(args, "from"), opts);
                Json arr = Json::array();
                for (const auto& r : results) {
                    Json o = Json::object();
                    o["uri"] = r.uri;
                    o["matchedName"] = r.matchedName;
                    o["tier"] = std::string(tierToString(r.tier));
                    o["distance"] = r.distance;
                    arr.push_back(std::move(o));
                }
                Json body = textContent(
                    std::to_string(results.size()) + " skill(s) found");
                body["results"] = std::move(arr);
                return body;
            });

        registerTool("listSkills",
            "List the available skills catalog.",
            Json::object(), [this](const Json& args) -> Json {
                auto entries = skill_.list(optStr(args, "scope"),
                    optStr(args, "version"), optStr(args, "from"));
                Json arr = Json::array();
                for (const auto& e : entries) {
                    Json o = Json::object();
                    o["uri"] = e.uri;
                    o["title"] = e.title;
                    Json names = Json::array();
                    for (const auto& n : e.names) names.push_back(n);
                    o["names"] = std::move(names);
                    arr.push_back(std::move(o));
                }
                Json body = textContent(
                    std::to_string(entries.size()) + " skill(s)");
                body["skills"] = std::move(arr);
                return body;
            });

        registerTool("getSkills",
            "Fetch skill payloads by cja-skill:// URI.",
            Json::object(), [this](const Json& args) -> Json {
                if (!args.isObject() || !args.has("uris")
                        || !args.at("uris").isArray()) {
                    throw McpError{-32602, "getSkills requires array 'uris'"};
                }
                const Json& uriArr = args.at("uris");
                std::vector<std::string> uris;
                for (size_t i = 0; i < uriArr.size(); i++) {
                    if (uriArr[i].isString()) uris.push_back(uriArr[i].asString());
                }
                auto results = skill_.get(uris);
                Json arr = Json::array();
                for (const auto& r : results) {
                    Json o = Json::object();
                    o["uri"] = r.uri;
                    o["ok"] = r.ok();
                    if (r.ok()) o["payload"] = r.payload;
                    else o["error"] = r.error;
                    arr.push_back(std::move(o));
                }
                Json body = textContent(
                    std::to_string(results.size()) + " result(s)");
                body["skills"] = std::move(arr);
                return body;
            });
    }

    void McpServer::registerTool(const std::string& name,
                                 const std::string& description, Json inputSchema,
                                 std::function<Json(const Json&)> call) {
        tools_[name] = Tool{name, description, std::move(inputSchema),
                            std::move(call)};
    }

    Json McpServer::initializeResult() const {
        Json caps = Json::object();
        caps["tools"] = Json::object();   // tools capability advertised
        Json info = Json::object();
        info["name"] = kServerName;
        info["version"] = kServerVersion;
        Json result = Json::object();
        result["protocolVersion"] = kProtocolVersion;
        result["capabilities"] = std::move(caps);
        result["serverInfo"] = std::move(info);
        return result;
    }

    Json McpServer::toolsList() const {
        Json arr = Json::array();
        for (const auto& [name, t] : tools_) {
            Json td = Json::object();
            td["name"] = t.name;
            td["description"] = t.description;
            td["inputSchema"] = t.inputSchema;
            arr.push_back(std::move(td));
        }
        Json body = Json::object();
        body["tools"] = std::move(arr);
        return body;
    }

    Json McpServer::callTool(const Json& params, const Json& id) {
        if (!params.isObject() || !params.has("name")) {
            return makeError(id, -32602, "tools/call requires a 'name'");
        }
        const std::string name = params.at("name").asString();
        auto it = tools_.find(name);
        if (it == tools_.end()) {
            return makeError(id, -32602, "Unknown tool: " + name);
        }
        Json args = params.has("arguments") ? params.at("arguments")
                                            : Json::object();
        try {
            return makeResult(id, it->second.call(args));
        } catch (const McpError& e) {
            return makeError(id, e.code, e.message);
        }
    }

    Json McpServer::handle(const Json& request) {
        if (!request.isObject() || !request.has("method")) {
            return makeError(Json(), -32600, "Invalid Request");
        }
        const bool isNotification = !request.has("id");
        const Json id = isNotification ? Json() : request.at("id");
        const std::string method = request.at("method").asString();

        if (method == "initialize") {
            return makeResult(id, initializeResult());
        }
        // Lifecycle notifications: accept silently, no response.
        if (method == "notifications/initialized" || method == "initialized") {
            return Json();
        }
        if (method == "tools/list") {
            return makeResult(id, toolsList());
        }
        if (method == "tools/call") {
            const Json params = request.has("params") ? request.at("params")
                                                      : Json::object();
            return callTool(params, id);
        }
        // Unknown notifications are ignored; unknown requests are an error.
        if (isNotification) return Json();
        return makeError(id, -32601, "Method not found: " + method);
    }

    int McpServer::run(std::istream& in, std::ostream& out) {
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            bool ok = false;
            Json request = Json::parse(line, &ok);
            if (!ok) {
                out << makeError(Json(), -32700, "Parse error").dump() << "\n";
                out.flush();
                continue;
            }
            Json response = handle(request);
            if (!response.isNull()) {
                out << response.dump() << "\n";
                out.flush();
            }
        }
        return 0;
    }

    int dispatchMcp(int argc, const char* argv[]) {
        McpServer server;
        server.setCompilerExePath(resolveSelfExe(argc > 0 ? argv[0] : nullptr));
        return server.run(std::cin, std::cout);
    }

} // namespace cajeta::mcp
