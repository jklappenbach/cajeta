# minimum required CMAKE version
project(cajeta)
CMAKE_MINIMUM_REQUIRED(VERSION 3.7 FATAL_ERROR)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# compiler must be 11 or 14
set(CMAKE_CXX_STANDARD 17)
set(Java_JAVA_EXECUTABLE "java")
set(ANTLR4_TAG e4c1a74)
#set(ANTLR4_TAG 828a6)

# required if linking to static library
add_definitions(-DANTLR4CPP_STATIC)

# using /MD flag for antlr4_runtime (for Visual C++ compilers only)
set(ANTLR4_WITH_STATIC_CRT OFF)
# add external build for antlrcpp
include(ExternalAntlr4Cpp)
# add antrl4cpp artifacts to project environment
include_directories(${ANTLR4_INCLUDE_DIRS})

# set variable pointing to the antlr tool that supports C++
# this is not required if the jar file can be found under PATH environment
set(ANTLR_EXECUTABLE /Users/julian/code/antlr4/tool/target/antlr4-4.9.4-SNAPSHOT-complete.jar)

# add macros to generate ANTLR Cpp code from grammar
find_package(ANTLR REQUIRED)

# Call macro to add lexer and grammar to your build dependencies.
antlr_target(CajetaGrammarLexer ${CMAKE_CURRENT_SOURCE_DIR}/antlr4/CajetaLexer.g4 LEXER
             PACKAGE cajeta)
antlr_target(CajetaGrammarParser ${CMAKE_CURRENT_SOURCE_DIR}/antlr4/CajetaParser.g4 PARSER VISITOR
             PACKAGE cajeta
             DEPENDS_ANTLR CajetaGrammarLexer
             COMPILE_FLAGS -lib ${ANTLR_CajetaGrammarLexer_OUTPUT_DIR})

# include generated files in project environment
include_directories(${ANTLR_CajetaGrammarLexer_OUTPUT_DIR})
include_directories(${ANTLR_CajetaGrammarParser_OUTPUT_DIR})

# add generated grammar to demo binary target
add_executable(cajeta
        ${ANTLR_CajetaGrammarLexer_CXX_OUTPUTS}
        ${ANTLR_CajetaGrammarParser_CXX_OUTPUTS}
        src/main.cpp)
target_link_libraries(cajeta antlr4_static)
