// cajetadoc — shared CLI entry point.
//
// One implementation drives both the standalone `cajetadoc` binary and the
// `cajeta doc` subcommand (which forwards here). argv[0] is treated as the
// program/command name; option parsing starts at argv[1]. Returns a process
// exit code.
#ifndef CAJETADOC_CLI_H
#define CAJETADOC_CLI_H

namespace cajetadoc {

int runCli(int argc, const char* const* argv);

} // namespace cajetadoc

#endif // CAJETADOC_CLI_H
