//
// Created by James Klappenbach on 1/19/22.
//

#include "TestCppClass.hpp"

TestCppClass::TestCppClass() {
    this->alpha = 50;
}

int TestCppClass::getAlpha() {
    return alpha;
}

int TestCppClass::main(int argc, char** argv) {
    TestCppClass* instance = new TestCppClass;
    delete instance;
    return 0;
}



