# libcajeta_amdtex — optional shared library wrapping AMD's (vendored, MIT) addrlib
# for the HIP mipmap/cubemap texture emulation. dlopen'd at runtime by
# cajeta_runtime.c (NOT linked) so the heavy C++ stays out of the embedded JIT
# bitcode and the path degrades to "unsupported" wherever the .so / an AMD GPU is
# absent. Linux-only (ROCm/HIP is Linux-only for cajeta's purposes).
#
# Building it doesn't pull in HIP or ROCm — addrlib is pure host-side address math.
# The runtime locates the .so beside the cajeta binary, via $CAJETA_AMD_AMDTEX_LIB,
# or by SONAME (see cajeta_xpu_load_amdtex).

# Linux-only: the vendored addrlib's Apple path (#if __APPLE__ in addrtypes.h)
# pulls a non-vendored `addrExtDef.h` maintained by Apple's driver team, so it
# can't build on macOS — and Apple has no AMD/HIP/ROCm to emulate anyway. amdtex
# is optional (dlopen'd), so skipping it there just leaves the feature absent.
if(UNIX AND NOT APPLE)
    set(_cajeta_amdtex_default ON)
else()
    set(_cajeta_amdtex_default OFF)
endif()
option(CAJETA_BUILD_AMDTEX
       "Build libcajeta_amdtex (AMD HIP mipmap/cube texture emulation helper)"
       ${_cajeta_amdtex_default})

if(NOT CAJETA_BUILD_AMDTEX OR NOT UNIX OR APPLE)
    return()
endif()

set(_addrlib ${CMAKE_CURRENT_SOURCE_DIR}/addrlib)
file(GLOB_RECURSE _addrlib_srcs CONFIGURE_DEPENDS ${_addrlib}/src/*.cpp)

add_library(cajeta_amdtex SHARED
    cajeta_amdtex.cpp
    ${_addrlib_srcs})

# addrlib's internal headers are spread across src/{core,gfx9,gfx10,gfx11,gfx12}
# and src/chip/*; the public API is under inc/.
target_include_directories(cajeta_amdtex PRIVATE
    ${_addrlib}/inc
    ${_addrlib}/src
    ${_addrlib}/src/core
    ${_addrlib}/src/gfx9
    ${_addrlib}/src/gfx10
    ${_addrlib}/src/gfx11
    ${_addrlib}/src/gfx12
    ${_addrlib}/src/chip/gfx9
    ${_addrlib}/src/chip/gfx10
    ${_addrlib}/src/chip/gfx11
    ${_addrlib}/src/chip/gfx12
    ${_addrlib}/src/chip/r800)

target_compile_definitions(cajeta_amdtex PRIVATE LITTLEENDIAN_CPU=1)
target_compile_features(cajeta_amdtex PRIVATE cxx_std_17)
set_target_properties(cajeta_amdtex PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    # Drop it beside the cajeta/cajeta_test binaries so the runtime's
    # "next to the executable" dlopen search finds it in a build tree.
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# addrlib is third-party (MIT, AMD); don't fail the build on its warnings.
target_compile_options(cajeta_amdtex PRIVATE -w)

install(TARGETS cajeta_amdtex
        LIBRARY DESTINATION lib COMPONENT cajeta)
