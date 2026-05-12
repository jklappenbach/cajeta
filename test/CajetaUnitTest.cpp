//
// Created by James Klappenbach on 11/18/23.
//
#include "CajetaUnitTest.h"
#include <cstdlib>

#define CAJETA_SOURCE_ROOT_KEY "CAJETA_SOURCE_ROOT"

static std::string lookupSourceRoot() {
    const char* env = std::getenv(CAJETA_SOURCE_ROOT_KEY);
    return env ? std::string(env) : std::string();
}

string CAJETA_SOURCE_ROOT = lookupSourceRoot();
string CAJETA_TEST_ROOT = CAJETA_SOURCE_ROOT + string("/test");
