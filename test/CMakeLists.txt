set(BINARY ${CMAKE_PROJECT_NAME}_test)

file(GLOB_RECURSE TEST_SOURCES LIST_DIRECTORIES false *.h *.cpp)
message("-- TEST_SOURCES: ${TEST_SOURCES}")

set(SOURCES ${TEST_SOURCES})
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR}/cmake)

#set(ANTLR_EXECUTABLE $ENV{ANTLR_EXECUTABLE})
#find_package(glog 0.6.0 REQUIRED)
#find_package(ANTLR REQUIRED)
#find_package(LLVM REQUIRED CONFIG)

include(FetchContent)
FetchContent_Declare(
        googletest
        # Specify the commit you depend on and update it regularly.
        URL https://github.com/google/googletest/archive/5376968f6948923e2411081fd9372e71a59d8e77.zip
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

#add_definitions(${LLVM_DEFINITIONS})
#include_directories(${LLVM_INCLUDE_DIRS})
#include_directories(${ANTLR4_INCLUDE_DIRS})
#include_directories("/usr/local/include/antlr4-runtime")
#include_directories("${CMAKE_SOURCE_DIR}/src")
#include_directories("/usr/local/include")
#include_directories("${CMAKE_SOURCE_DIR}/generated/lex")
#include_directories("${CMAKE_SOURCE_DIR}/generated/parser")

add_executable(${BINARY} ${TEST_SOURCES})

target_link_libraries(${BINARY} ${CMAKE_PROJECT_NAME}_lib gtest_main)

add_test(NAME ${BINARY} COMMAND ${BINARY})

