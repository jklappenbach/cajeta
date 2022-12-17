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
    int* array;
};

int TestCppClass::main(int argc, char** argv) {
    Blah* blah = new Blah;
    blah->x = 5;
    blah->array = new int[5];
    blah->array[0] = 10;
    blah->array[1] = 11;
    blah->array[2] = 12;

//    Blah* blah = new Blah;
//    blah->instance = new TestCppClass;
//    blah->x = 5;
    delete blah->array;
    delete blah;
    return 0;
}



