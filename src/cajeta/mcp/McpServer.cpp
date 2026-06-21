#include "cajeta/mcp/McpServer.h"

#include <sstream>

namespace cajeta::mcp {

    namespace {

        // MCP protocol revision this server implements.
        constexpr const char* kProtocolVersion = "2024-11-05";
        constexpr const char* kServerName = "cajeta";
        constexpr const char* kServerVersion = "0.1.0";

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

    McpServer::McpServer() {
        // U1 registers all five spec tools as stubs so tools/list is complete;
        // later units replace the handlers with real implementations.
        Json objectSchema = Json::object();
        objectSchema["type"] = "object";
        auto stub = [](const Json&) -> Json {
            return textContent("not implemented", /*isError=*/true);
        };
        registerTool("searchSkills",
            "Search skills by canonical name (fuzzy, typo-tolerant).",
            objectSchema, stub);
        registerTool("getSkills",
            "Fetch skill payloads by cja-skill:// URI.",
            objectSchema, stub);
        registerTool("listSkills",
            "List the available skills catalog.",
            objectSchema, stub);
        registerTool("compile",
            "Compile a base64 tar.zstd source archive; return diagnostics.",
            objectSchema, stub);
        registerTool("jit_execute",
            "Compile and JIT-execute a source archive (process-per-execute).",
            objectSchema, stub);
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
