#include <iostream>
#include <llvm/Support/InitLLVM.h>
#include "cajeta/compile/Compiler.h"

using namespace std;
using namespace antlr4;
using namespace cajeta;

#define PATH_SEPARATOR_CHAR '/'

int main(int argc, const char* argv[]) {
    //cl::ParseCommandLineOptions(argc, argv, " Cajeta compiler, v1.0\n");
    Compiler compiler(argc, argv);
    compiler.compile(argv[1], argv[2], argv[3]);
    return 0;
}
