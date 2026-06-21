//
// cajeta-mcp — standalone MCP server that wraps the `cajeta` compiler.
//
// Speaks JSON-RPC 2.0 over stdio; implements its tools by shelling out to
// `cajeta` subcommands. The compiler binary is located via --cajeta=PATH,
// $CAJETA_BIN, or "cajeta" on PATH.
//
#include "McpServer.h"

int main(int argc, const char* argv[]) {
    return cajeta::mcp::dispatchMcp(argc, argv);
}
