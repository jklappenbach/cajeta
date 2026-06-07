// cajetadoc — standalone binary entry point.
//
// Thin wrapper over the shared CLI (cajetadoc::runCli), which also backs the
// `cajeta doc` subcommand. See src/Cli.cpp for the actual logic.
#include "cajetadoc/Cli.h"

int main(int argc, char** argv) { return cajetadoc::runCli(argc, argv); }
