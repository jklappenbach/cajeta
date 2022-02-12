//
// Created by James Klappenbach on 1/19/22.
//

#ifndef CAJETA_TESTCPPCLASS_HPP
#define CAJETA_TESTCPPCLASS_HPP

#include <string>

class TestCppClass {
private:
    int alpha;
public:
    TestCppClass();

    int getAlpha();
    static int main(int argc, char** argv);
};

#endif //CAJETA_TESTCPPCLASS_HPP
