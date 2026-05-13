//
// Created by James Klappenbach on 11/18/23.
//
#include "CajetaUnitTest.h"
#include <cstdlib>

#define CAJETA_SOURCE_ROOT_KEY "CAJETA_SOURCE_ROOT"

static std::string lookupSourceRoot() {
    // Env var takes precedence (lets a CI / developer override the baked
    // default without recompiling). Otherwise fall back to CAJETA_SOURCE_ROOT_DEFAULT
    // which is set at build time by CMake to PROJECT_SOURCE_DIR — so a
    // bare `./build/test/cajeta_test` invocation Just Works without
    // requiring the env var to be set.
    const char* env = std::getenv(CAJETA_SOURCE_ROOT_KEY);
    if (env && *env) return std::string(env);
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
    return std::string(CAJETA_SOURCE_ROOT_DEFAULT);
#else
    return std::string();
#endif
}

string CAJETA_SOURCE_ROOT = lookupSourceRoot();
string CAJETA_TEST_ROOT = CAJETA_SOURCE_ROOT + string("/test");
