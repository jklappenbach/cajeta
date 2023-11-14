set(BINARY ${CMAKE_PROJECT_NAME}_test)

file(GLOB_RECURSE TEST_SOURCES LIST_DIRECTORIES false *.h *.cpp)
message("-- TEST_SOURCES: ${TEST_SOURCES}")

set(SOURCES ${TEST_SOURCES})
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR}/cmake)

set(ANTLR_EXECUTABLE $ENV{ANTLR_EXECUTABLE})
find_package(glog 0.6.0 REQUIRED)
find_package(GTest REQUIRED)
find_package(ANTLR REQUIRED)
find_package(LLVM REQUIRED CONFIG)

add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})
include_directories(${ANTLR4_INCLUDE_DIRS})
include_directories("/usr/local/include/antlr4-runtime")
include_directories("${CMAKE_SOURCE_DIR}/src")
include_directories("/usr/local/include")
include_directories("${CMAKE_SOURCE_DIR}/generated/lex")
include_directories("${CMAKE_SOURCE_DIR}/generated/parser")

add_executable(${BINARY} ${TEST_SOURCES})

add_test(NAME ${BINARY} COMMAND ${BINARY})

target_link_libraries(${BINARY} PUBLIC ${CMAKE_PROJECT_NAME}_lib GTest)
