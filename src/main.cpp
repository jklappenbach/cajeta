#include <iostream>
#include <string>
#include <vector>
#include <llvm/Support/InitLLVM.h>
#include "cajeta/compile/Compiler.h"

using namespace std;
using namespace antlr4;
using namespace cajeta;

namespace {

void printUsage(const char* progname) {
    std::cerr << "Usage: " << progname
              << " [options] <entry-method> <source-root-path> <archive-root-path>\n"
              << "\n"
              << "Options:\n"
              << "  --bounds=on|off    Enable or disable array bounds checking. Default: on.\n"
              << "  --emit=ir|obj|exe  Output mode. Default: ir (.ll text). obj emits native\n"
              << "                     object files. exe links via lld (when built with lld libs).\n"
              << "  --target=<triple>  LLVM target triple (e.g. aarch64-linux-gnu). Default: host.\n"
              << "  --cpu=<name>       Target CPU (e.g. cortex-a72). Default: generic.\n"
              << "  --features=<list>  Comma-separated target features (e.g. +neon,+fp-armv8).\n"
              << "  -o <path>          Output path for the final artifact (exe mode primarily).\n"
              << "  --help, -h         This message.\n";
}

// Helper: consume `--name=value` or split a positional `--name value` pair.
// Returns the value or empty if absent / malformed.
bool startsWith(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

} // namespace

int main(int argc, const char* argv[]) {
    Compiler compiler(argc, argv);
    std::vector<std::string> positional;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (startsWith(arg, "--bounds=")) {
            std::string value = arg.substr(std::string("--bounds=").size());
            if (value == "off" || value == "false" || value == "0") {
                compiler.setBoundsCheckEnabled(false);
            } else if (value == "on" || value == "true" || value == "1") {
                compiler.setBoundsCheckEnabled(true);
            } else {
                std::cerr << "cajeta: unrecognized value for --bounds: " << value << "\n";
                printUsage(argv[0]);
                return 1;
            }
        } else if (startsWith(arg, "--emit=")) {
            std::string value = arg.substr(std::string("--emit=").size());
            if (value == "ir") {
                compiler.setEmitMode(EmitMode::IR);
            } else if (value == "obj") {
                compiler.setEmitMode(EmitMode::Obj);
            } else if (value == "exe") {
                compiler.setEmitMode(EmitMode::Exe);
            } else {
                std::cerr << "cajeta: unrecognized value for --emit: " << value
                          << " (expected ir|obj|exe)\n";
                printUsage(argv[0]);
                return 1;
            }
        } else if (startsWith(arg, "--target=")) {
            compiler.setTargetTriple(arg.substr(std::string("--target=").size()));
        } else if (startsWith(arg, "--cpu=")) {
            compiler.setCpu(arg.substr(std::string("--cpu=").size()));
        } else if (startsWith(arg, "--features=")) {
            compiler.setFeatures(arg.substr(std::string("--features=").size()));
        } else if (arg == "-o") {
            if (i + 1 >= argc) {
                std::cerr << "cajeta: -o requires a path argument\n";
                return 1;
            }
            compiler.setOutputPath(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (startsWith(arg, "--")) {
            std::cerr << "cajeta: unknown option: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        } else {
            positional.push_back(arg);
        }
    }

    if (positional.size() < 3) {
        printUsage(argc > 0 ? argv[0] : "cajeta");
        return 1;
    }

    compiler.compile(positional[0], positional[1], positional[2]);
    return 0;
}
