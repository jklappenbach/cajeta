//
// MCP (Model Context Protocol) server for the cajeta toolchain — a STANDALONE
// tool (tools/mcp) that WRAPS the `cajeta` compiler binary: it speaks JSON-RPC
// 2.0 over stdio (newline-delimited) and implements every tool by shelling out
// to a `cajeta` subcommand. It does NOT link the compiler (cajeta_lib); it only
// reuses a few small utility TUs (Json, Subprocess, TarZstd). See
// docs/specs/cajeta-mcp-spec.md.
//
#pragma once

#include "cajeta/dap/Json.h"

#include <functional>
#include <iostream>
#include <map>
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

    // The outcome of running a `cajeta` subcommand.
    struct CliResult {
        bool launched = false;
        int exitCode = -1;
        std::string out;
        std::string err;
    };

    // Runs `cajeta <args...>` (the exe is prepended by the runner) and returns
    // its result. Injectable so tests stub it with canned output.
    using CliRunner = std::function<CliResult(const std::vector<std::string>& args)>;

    class McpServer {
    public:
        McpServer();

        // Read newline-delimited JSON-RPC requests from `in`, write responses to
        // `out`. Returns 0 at EOF.
        int run(std::istream& in, std::ostream& out);

        // Handle one parsed request. Returns the response object, or a Null Json
        // for a notification (no `id`).
        Json handle(const Json& request);

        // Path to the `cajeta` executable the tools shell out to. Defaults to
        // this process's own exe's sibling resolution; production sets it.
        void setCompilerExePath(std::string path) {
            compilerExePath_ = std::move(path);
        }

        // Override the CLI runner (tests inject canned results).
        void setCliRunner(CliRunner runner) { cli_ = std::move(runner); }

    private:
        struct Tool {
            std::string name;
            std::string description;
            Json inputSchema;
            std::function<Json(const Json& args)> call;
        };

        void registerTool(const std::string& name, const std::string& description,
                          Json inputSchema, std::function<Json(const Json&)> call);
        void registerTools();
        void installDefaultRunner();

        Json initializeResult() const;
        Json toolsList() const;
        Json callTool(const Json& params, const Json& id);

        // Run a skill subcommand with --json and wrap its parsed array under
        // `field` in the tool result.
        Json runSkillTool(std::vector<std::string> args, const char* field);

        std::map<std::string, Tool> tools_;
        CliRunner cli_;
        std::string compilerExePath_;
    };

    // `cajeta-mcp` entry point.
    int dispatchMcp(int argc, const char* argv[]);

} // namespace cajeta::mcp
