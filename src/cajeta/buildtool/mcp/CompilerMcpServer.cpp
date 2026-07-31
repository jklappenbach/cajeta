// See CompilerMcpServer.h. JSON-RPC parsing/shaping uses the dap::Json model;
// tool dispatch lands with the skill tools (plan Unit 3).

#include "cajeta/buildtool/mcp/CompilerMcpServer.h"

#include "cajeta/buildtool/ArtifactCache.h"
#include "cajeta/buildtool/Lockfile.h"
#include "cajeta/buildtool/skill/SkillCli.h"
#include "cajeta/dap/Json.h"

#include <filesystem>
#include <istream>
#include <ostream>
#include <utility>
#include <vector>

namespace cajeta::buildtool::mcp {

    using cajeta::dap::Json;

    namespace {

        // Kept content-identical to the cajeta-written server's initialize
        // instructions (tools/mcp/src/main/cajeta/mcp/Server.cajeta) — update
        // both together.
        constexpr const char* kInstructions =
            "This server provides skills: authoritative, hand-written "
            "implementation guides for cajeta libraries. Before you write or "
            "edit cajeta code that uses a library, package, class, or method, "
            "first call searchSkills with its name (fuzzy and typo-tolerant). "
            "If a relevant skill is returned, call getSkills to fetch its "
            "payload and follow that guidance over your prior assumptions. Use "
            "listSkills to browse what is available, and follow a skill's "
            "references to related skills.";

        constexpr const char* kProtocolVersion = "2024-11-05";

        Json envelope(const Json& id) {
            Json r = Json::object();
            r["jsonrpc"] = "2.0";
            r["id"] = id;
            return r;
        }

        std::string errorResponse(const Json& id, int code,
                                  const std::string& message) {
            Json r = envelope(id);
            Json e = Json::object();
            e["code"] = code;
            e["message"] = message;
            r["error"] = e;
            return r.dump();
        }

        std::string resultResponse(const Json& id, Json result) {
            Json r = envelope(id);
            r["result"] = std::move(result);
            return r.dump();
        }

        Json toolDesc(const char* name, const char* desc, Json properties,
                      std::vector<const char*> required) {
            Json schema = Json::object();
            schema["type"] = "object";
            schema["properties"] = std::move(properties);
            if (!required.empty()) {
                Json req = Json::array();
                for (const char* r : required) req.push_back(r);
                schema["required"] = req;
            }
            Json t = Json::object();
            t["name"] = name;
            t["description"] = desc;
            t["inputSchema"] = std::move(schema);
            return t;
        }

        Json prop(const char* type, const char* desc) {
            Json p = Json::object();
            p["type"] = type;
            p["description"] = desc;
            return p;
        }

        Json toolsList() {
            Json search = Json::object();
            search["name"] = prop("string",
                "Canonical name to resolve: library (cajeta.io), package "
                "(cajeta/io/file), class, or method. Fuzzy, typo-tolerant.");
            search["version"] = prop("string", "Restrict to a resolved version.");
            search["from"] = prop("string",
                "Resolve versions as this module sees them.");
            Json exact = Json::object();
            exact["type"] = "boolean";
            exact["description"] = "Disable fuzzy matching.";
            search["exact"] = exact;

            Json list = Json::object();
            list["scope"] = prop("string",
                "Library or package subtree to enumerate (slash form, e.g. "
                "cajeta/toolchain); omit for all skills.");
            list["version"] = prop("string", "Restrict to a resolved version.");
            list["from"] = prop("string",
                "Resolve versions as this module sees them.");

            Json uris = Json::object();
            uris["type"] = "array";
            Json items = Json::object();
            items["type"] = "string";
            uris["items"] = items;
            uris["description"] = "cja-skill:// URIs to fetch (batch).";
            Json get = Json::object();
            get["uris"] = uris;

            Json tools = Json::array();
            tools.push_back(toolDesc("searchSkills",
                "Search skills by canonical name (fuzzy, typo-tolerant).",
                std::move(search), {"name"}));
            tools.push_back(toolDesc("listSkills",
                "List the available skills catalog.", std::move(list), {}));
            tools.push_back(toolDesc("getSkills",
                "Fetch skill payloads by cja-skill:// URI.", std::move(get),
                {"uris"}));
            Json result = Json::object();
            result["tools"] = std::move(tools);
            return result;
        }

        bool isKnownTool(const std::string& name) {
            return name == "searchSkills" || name == "listSkills" ||
                   name == "getSkills";
        }

    } // namespace

    llvm::Expected<CompilerMcpServer>
    CompilerMcpServer::create(std::string version, std::string projectDir) {
        namespace fs = std::filesystem;
        // Lockfile optional (discovery spec §2.5): embedded corpora always seed.
        std::vector<ResolvedPackageEntry> packages;
        fs::path lock = fs::path(projectDir) / "cajeta.lock";
        if (fs::exists(lock)) {
            auto lf = readLockfile(lock.string());
            if (!lf) return lf.takeError();
            packages = std::move(lf->packagesTyped);
        }
        ArtifactCache cache(projectDir);
        auto ctx = skill::loadSkillSearchContext(
            packages, [&](llvm::StringRef s) { return cache.lookup(s.str()); });
        if (!ctx) return ctx.takeError();
        return CompilerMcpServer(std::move(version), std::move(*ctx));
    }

    llvm::StringRef CompilerMcpServer::instructions() { return kInstructions; }

    std::optional<std::string>
    CompilerMcpServer::handleMessage(llvm::StringRef message) {
        bool ok = false;
        Json req = Json::parse(message.str(), &ok);
        if (!ok) return errorResponse(Json(), -32700, "parse error");
        if (!req.isObject())
            return errorResponse(Json(), -32600, "request must be an object");

        const Json& id = req.at("id");
        const bool isNotification = !req.has("id");
        const std::string& method = req.at("method").asString();

        if (method.empty()) {
            if (isNotification) return std::nullopt;
            return errorResponse(id, -32600, "missing method");
        }
        if (isNotification) return std::nullopt; // e.g. notifications/initialized

        if (method == "initialize") {
            Json result = Json::object();
            result["protocolVersion"] = kProtocolVersion;
            Json caps = Json::object();
            caps["tools"] = Json::object();
            result["capabilities"] = caps;
            Json info = Json::object();
            info["name"] = "compiler-mcp";
            info["version"] = version_;
            result["serverInfo"] = info;
            result["instructions"] = kInstructions;
            return resultResponse(id, std::move(result));
        }
        if (method == "tools/list") return resultResponse(id, toolsList());
        if (method == "tools/call") {
            const std::string& tool = req.at("params").at("name").asString();
            if (!isKnownTool(tool))
                return errorResponse(id, -32602, "unknown tool: " + tool);
            return errorResponse(id, -32603,
                                 "tool not yet wired: " + tool); // Unit 3
        }
        return errorResponse(id, -32601, "method not found: " + method);
    }

    int CompilerMcpServer::run(std::istream& in, std::ostream& out) {
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            if (auto resp = handleMessage(line)) {
                out << *resp << "\n";
                out.flush();
            }
        }
        return 0;
    }

} // namespace cajeta::buildtool::mcp
