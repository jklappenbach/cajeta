CMAKE_MINIMUM_REQUIRED(VERSION 3.5)

LIST(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)

# compiler must be 11 or 14
set(CMAKE_CXX_STANDARD 17)
cmake_policy(SET CMP0135 NEW)
cmake_policy(SET CMP0075 NEW)


# set variable pointing to the antlr tool that supports C++
set(Java_JAVA_EXECUTABLE "java")
set(ANTLR4CPP_JAR_LOCATION $ENV{ANTLR_EXECUTABLE})
set(ANTLR_EXECUTABLE $ENV{ANTLR_EXECUTABLE})
add_definitions(-DANTLR4CPP_STATIC)
set(ANTLR4_TAG "7ed420f")
set(ANTLR4_INSTALL ON)

# add external build for antlrcpp
include(ExternalAntlr4Cpp)
# add antrl4cpp artifacts to project environment
link_directories(${ANTLR4CPP_LIBS})

find_package(glog 0.6.0 REQUIRED)
find_package(GTest 1.12.0 REQUIRED)
find_package(LLVM REQUIRED CONFIG)
add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})
find_package(ANTLR REQUIRED)
message(STATUS "LLVM_DEFINITIONS: ${LLVM_DEFINITIONS}")
include_directories(${ANTLR4CPP_INCLUDE_DIRS})
include_directories("/usr/local/include")
include_directories("/usr/local/include/antlr4-runtime")

antlr_target(CajetaGrammarLexer ${CMAKE_SOURCE_DIR}/antlr4/CajetaLexer.g4 LEXER
        PACKAGE cajeta OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/generated/lex")
antlr_target(CajetaGrammarParser ${CMAKE_SOURCE_DIR}/antlr4/CajetaParser.g4 PARSER VISITOR
        PACKAGE cajeta
        DEPENDS_ANTLR CajetaGrammarLexer
        COMPILE_FLAGS -lib ${ANTLR_CajetaGrammarLexer_OUTPUT_DIR}
        OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/generated/parser")

## include generated files in project environment
include_directories(${ANTLR_CajetaGrammarLexer_OUTPUT_DIR})
include_directories(${ANTLR_CajetaGrammarParser_OUTPUT_DIR})

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
message(STATUS "Using LLVM includes at: ${LLVM_INCLUDE_DIRS}")
message(STATUS "Using project includes at: ${CMAKE_INCLUDE_SOURCE_DIR}/include")
message(STATUS "ANTLR4CPP_INCLUDE_DIRS: ${ANTLR4CPP_INCLUDE_DIRS}")

set(LLVM_TARGETS_TO_BUILD ${LLVM_ALL_TARGETS})
foreach(target ${LLVM_TARGETS_TO_BUILD})
    list(APPEND targets "LLVM${target}CodeGen")
endforeach()

set(BINARY ${CMAKE_PROJECT_NAME})
file(GLOB_RECURSE SOURCES LIST_DIRECTORIES true *.h *.cpp)
set(SOURCES ${ANTLR_CajetaGrammarLexer_CXX_OUTPUTS} ${ANTLR_CajetaGrammarParser_CXX_OUTPUTS} ${SOURCES})
add_executable(${CMAKE_PROJECT_NAME} ${SOURCES})
add_library(${CMAKE_PROJECT_NAME}_lib STATIC ${SOURCES})

target_link_libraries(${CMAKE_PROJECT_NAME}
        antlr4_static
        pthread
        LLVMX86AsmParser # MC, MCParser, Support, X86Desc, X86Info
        ${targets}
        LLVMOption # Support
        glog::glog)
