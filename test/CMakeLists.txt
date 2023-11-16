set(BINARY ${CMAKE_PROJECT_NAME}_test)

file(GLOB_RECURSE TEST_SOURCES LIST_DIRECTORIES false *.h *.cpp)
message("-- TEST_SOURCES: ${TEST_SOURCES}")

set(SOURCES ${TEST_SOURCES})
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR}/cmake)

#set(ANTLR_EXECUTABLE $ENV{ANTLR_EXECUTABLE})
#find_package(glog 0.6.0 REQUIRED)
#find_package(ANTLR REQUIRED)
#find_package(LLVM REQUIRED CONFIG)

cmake_policy(SET CMP0135 NEW)
cmake_policy(SET CMP0075 NEW)

find_package(glog 0.6.0 REQUIRED)

include(FetchContent)
FetchContent_Declare(
        googletest
        # Specify the commit you depend on and update it regularly.
        URL https://github.com/google/googletest/archive/5376968f6948923e2411081fd9372e71a59d8e77.zip
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

find_package(LLVM REQUIRED CONFIG)
include_directories(${LLVM_INCLUDE_DIRS})
#include_directories(${ANTLR4_INCLUDE_DIRS})
include_directories("/usr/local/include/antlr4-runtime")
include_directories("/usr/local/include")
include_directories("${CMAKE_SOURCE_DIR}/generated/lex")
include_directories("${CMAKE_SOURCE_DIR}/generated/parser")

add_executable(${BINARY} ${TEST_SOURCES})
message(STATUS "LLVM_: ${targets}")

target_link_libraries(${BINARY}
        ${CMAKE_PROJECT_NAME}_lib
        /usr/local/lib/libantlr4-runtime.a
        gtest_main
        LLVMAArch64CodeGen
        LLVMARMCodeGen
        LLVMAMDGPUCodeGen
        LLVMX86CodeGen
        LLVMX86AsmParser # MC, MCParser, Support, X86Desc, X86Info
        LLVMX86Desc # MC, Support, X86AsmPrinter, X86Info
        LLVMX86Info # MC, Support, Target
        LLVMAVRCodeGen
        LLVMBPFCodeGen
        LLVMHexagonCodeGen
        LLVMMSP430CodeGen
        LLVMLanaiCodeGen
        LLVMLoongArchCodeGen
        LLVMMipsCodeGen
        LLVMNVPTXCodeGen
        LLVMPowerPCCodeGen
        LLVMRISCVCodeGen
        LLVMSparcCodeGen
        LLVMSystemZCodeGen
        LLVMWebAssemblyCodeGen
        LLVMXCoreCodeGen
        LLVMVECodeGen
        LLVMMSP430Desc
        ${targets}
        LLVMOption # Support
        glog::glog)

add_test(NAME ${BINARY} COMMAND ${BINARY})

