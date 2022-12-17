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

struct Blah {
public:
    long x;
    TestCppClass* instance;
};

int TestCppClass::main(int argc, char** argv) {
    Blah* blah = new Blah;
    blah->instance = new TestCppClass;
    blah->x = 5;
    delete blah->instance;
    delete blah;
    return 0;
}



