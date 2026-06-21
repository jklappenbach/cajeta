#include "cajeta/mcp/McpServer.h"

#include "cajeta/buildtool/ArtifactCache.h"
#include "cajeta/buildtool/Lockfile.h"
#include "cajeta/buildtool/skill/SkillCli.h"

#include <llvm/Support/Error.h>

#include <sstream>

namespace cajeta::mcp {

    namespace {

        // MCP protocol revision this server implements.
        constexpr const char* kProtocolVersion = "2024-11-05";
        constexpr const char* kServerName = "cajeta";
        constexpr const char* kServerVersion = "0.1.0";

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
        installDefaultSkillBackend();
        registerSkillTools();

        // compile / jit_execute remain stubs until their units land; registering
        // them here keeps tools/list complete.
        Json objectSchema = Json::object();
        objectSchema["type"] = "object";
        auto stub = [](const Json&) -> Json {
            return textContent("not implemented", /*isError=*/true);
        };
        registerTool("compile",
            "Compile a base64 tar.zstd source archive; return diagnostics.",
            objectSchema, stub);
        registerTool("jit_execute",
            "Compile and JIT-execute a source archive (process-per-execute).",
            objectSchema, stub);
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
        (void) argc;
        (void) argv;
        McpServer server;
        return server.run(std::cin, std::cout);
    }

} // namespace cajeta::mcp
