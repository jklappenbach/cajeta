#include <iostream>
#include "cajeta/compile/Compiler.h"

using namespace std;
using namespace antlr4;
using namespace cajeta;

#define PATH_SEPARATOR_CHAR '/'

int main(int argc, const char* argv[]) {
    //cl::ParseCommandLineOptions(argc, argv, " Cajeta compiler, v1.0\n");

    Compiler compiler;
    compiler.compile(argv[1], argv[2]);
    return 0;
}
