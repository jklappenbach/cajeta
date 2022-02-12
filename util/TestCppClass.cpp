//
// Created by James Klappenbach on 1/19/22.
//

#include "TestCppClass.hpp"
#include <stdio.h>


TestCppClass::TestCppClass() {
    this->alpha = 50;
}

int TestCppClass::getAlpha() {
    return alpha;
}

int TestCppClass::main(int argc, char** argv) {
    printf("A basic method");
    return 0;
}



