set(BINARY ${CMAKE_PROJECT_NAME}_test)

file(GLOB_RECURSE TEST_SOURCES LIST_DIRECTORIES false *.h *.cpp)
message(STATUS "TEST_SOURCES: ${TEST_SOURCES}")

cmake_policy(SET CMP0135 NEW)
cmake_policy(SET CMP0075 NEW)

find_package(GTest REQUIRED)

add_executable(${BINARY} ${TEST_SOURCES})

target_link_libraries(${BINARY} PRIVATE
        ${CMAKE_PROJECT_NAME}_lib
        GTest::gtest
        GTest::gtest_main)

# Bake the project source root into the test binary so the disk-reading
# CompilerTests (`canParseOnValidShortPackage`, etc.) can find their .cajeta
# fixtures without requiring CAJETA_SOURCE_ROOT to be set in the environment.
# Tests still honor the env var when present (overrides the default).
target_compile_definitions(${BINARY} PRIVATE
        CAJETA_SOURCE_ROOT_DEFAULT="${PROJECT_SOURCE_DIR}")

include(GoogleTest)
gtest_discover_tests(${BINARY})
