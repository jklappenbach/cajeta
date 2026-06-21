//
// MCP (Model Context Protocol) server for the cajeta toolchain — JSON-RPC 2.0
// over stdio (newline-delimited messages). Exposes skill-discovery and
// compiler tools (see docs/specs/cajeta-mcp-spec.md).
//
// Modeled on the existing DAP server (src/cajeta/dap/) and reuses its hand-
// rolled Json value model. Framing differs: DAP uses Content-Length; MCP stdio
// is one JSON object per line.
//
#pragma once

#include "cajeta/dap/Json.h"
#include "cajeta/buildtool/skill/SkillGet.h"
#include "cajeta/buildtool/skill/SkillSearch.h"

#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cajeta::mcp {

    using cajeta::dap::Json;

    // Thrown by a tool handler to surface a JSON-RPC error (e.g. invalid params
    // -32602). callTool() catches it and builds the error response.
    struct McpError {
        int code;
        std::string message;
    };

    // The skill-discovery cores behind the searchSkills/listSkills/getSkills
    // tools, injected so the tool handlers stay pure marshalling. The default
    // backend loads `<root>/cajeta.lock` + the ArtifactCache and calls the real
    // cores; tests inject fixtures.
    struct SkillBackend {
        std::function<std::vector<buildtool::skill::SkillSearchResult>(
            const std::string& name, std::optional<std::string> version,
            std::optional<std::string> from,
            buildtool::skill::MatchOptions opts)> search;
        std::function<std::vector<buildtool::skill::SkillListEntry>(
            std::optional<std::string> scope, std::optional<std::string> version,
            std::optional<std::string> from)> list;
        std::function<std::vector<buildtool::skill::SkillGetResult>(
            const std::vector<std::string>& uris)> get;
    };

    class McpServer {
    public:
        McpServer();

        // Read newline-delimited JSON-RPC requests from `in`, write responses to
        // `out`. Returns 0 at EOF.
        int run(std::istream& in, std::ostream& out);

        // Handle one parsed request. Returns the response object, or a Null Json
        // for a notification (no `id`) — the caller writes nothing for Null.
        Json handle(const Json& request);

        // Project root the default skill backend reads cajeta.lock / artifacts
        // from (default "."). Set before serving.
        void setProjectRoot(std::string root) { projectRoot_ = std::move(root); }

        // Override the skill backend (tests inject fixtures).
        void setSkillBackend(SkillBackend backend);

    private:
        // A registered tool: its advertised schema + the handler that runs it.
        // The handler receives the call `arguments` object and returns the
        // tools/call result body (`{content:[...]}`); it throws McpError for a
        // protocol-level failure.
        struct Tool {
            std::string name;
            std::string description;
            Json inputSchema;
            std::function<Json(const Json& args)> call;
        };

        void registerTool(const std::string& name, const std::string& description,
                          Json inputSchema, std::function<Json(const Json&)> call);
        // (Re)install the three skill tools bound to the current backend.
        void registerSkillTools();
        // Bind skill_ to the default lockfile/ArtifactCache-backed cores.
        void installDefaultSkillBackend();

        Json initializeResult() const;
        Json toolsList() const;
        Json callTool(const Json& params, const Json& id);

        std::map<std::string, Tool> tools_;
        SkillBackend skill_;
        std::string projectRoot_ = ".";
    };

    // `cajeta mcp` subcommand entry point.
    int dispatchMcp(int argc, const char* argv[]);

} // namespace cajeta::mcp
