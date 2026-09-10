// compiler-mcp — the in-compiler MCP stdio server (specs/archive/compiler-mcp-spec.md):
// searchSkills / listSkills / getSkills over JSON-RPC 2.0, in-process and read-only.
#pragma once

#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>

#include "cajeta/buildtool/Lockfile.h"
#include "cajeta/buildtool/skill/SkillSearch.h"

namespace cajeta::dap { class Json; }

namespace cajeta::buildtool::mcp {

    class CompilerMcpServer {
    public:
        // Builds the discovery context exactly as the skill CLI does; a missing lockfile is not an error.
        static llvm::Expected<CompilerMcpServer> create(std::string version,
                                                        std::string projectDir);

        // One JSON-RPC message in → serialized response out; nullopt for notifications.
        std::optional<std::string> handleMessage(llvm::StringRef message);

        int run(std::istream& in, std::ostream& out);

        static llvm::StringRef instructions();

        size_t archiveCount() const { return ctx_.archives.size(); }

    private:
        CompilerMcpServer(std::string version, std::string projectDir,
                          std::vector<ResolvedPackageEntry> packages,
                          skill::SkillSearchContext ctx)
            : version_(std::move(version)), projectDir_(std::move(projectDir)),
              packages_(std::move(packages)), ctx_(std::move(ctx)) {}

        std::string handleToolCall(const cajeta::dap::Json& id,
                                   const cajeta::dap::Json& params);

        std::string version_;
        std::string projectDir_;                   // getSkills archive lookups
        std::vector<ResolvedPackageEntry> packages_;
        skill::SkillSearchContext ctx_;
    };

} // namespace cajeta::buildtool::mcp
