CMAKE_MINIMUM_REQUIRED(VERSION 3.25 FATAL_ERROR)

# Canonical version source: the VERSION file at the repo root. Bumped
# manually as part of the release process (see RELEASING.md). Baked
# into the compiler binary so `cajeta --version` reports it.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" CAJETA_VERSION)
string(STRIP "${CAJETA_VERSION}" CAJETA_VERSION)

project(cajeta VERSION ${CAJETA_VERSION})

cmake_policy(SET CMP0135 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)
set(CMAKE_CXX_STANDARD 17)
set(Java_JAVA_EXECUTABLE "java")

# Capture the short commit hash for `cajeta --version`. When git isn't
# available or the tree isn't a checkout (release tarball builds), fall
# back to "unknown".
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE CAJETA_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_RC)
    if(NOT GIT_RC EQUAL 0)
        set(CAJETA_GIT_HASH "unknown")
    endif()
else()
    set(CAJETA_GIT_HASH "unknown")
endif()

add_compile_definitions(
    CAJETA_VERSION="${CAJETA_VERSION}"
    CAJETA_GIT_HASH="${CAJETA_GIT_HASH}"
)

include_directories(src)

add_subdirectory(src)
add_subdirectory(test)
