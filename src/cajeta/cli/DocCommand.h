// `cajeta doc` — the documentation subcommand, forwarded to the cajetadoc CLI.
#ifndef CAJETA_CLI_DOCCOMMAND_H
#define CAJETA_CLI_DOCCOMMAND_H

namespace cajeta {
namespace doc {

// argv is the process argv (argv[1] == "doc"). Returns a process exit code.
int dispatchDoc(int argc, const char* const* argv);

} // namespace doc
} // namespace cajeta

#endif // CAJETA_CLI_DOCCOMMAND_H
