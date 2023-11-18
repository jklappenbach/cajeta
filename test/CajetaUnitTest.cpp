//
// Created by James Klappenbach on 11/18/23.
//
#include "CajetaUnitTest.h"
#include <cstdlib>

#define CAJETA_SOURCE_ROOT_KEY "CAJETA_SOURCE_ROOT"

string CAJETA_SOURCE_ROOT = std::string(std::getenv(CAJETA_SOURCE_ROOT_KEY));
string CAJETA_TEST_ROOT = CAJETA_SOURCE_ROOT + string("/test");