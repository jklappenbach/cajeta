//
// Which `ptxas` cajeta assembles with, and which ones it refuses.
//
// Why this exists: CUDA 12.0's ptxas (12.0.140, the one Ubuntu ships as the
// `nvidia-cuda-toolkit` package at /usr/bin/ptxas) MISCOMPILES the portable
// software CooperativeMatrix kernel. The emitted PTX is correct — the store
// base is computed in a block that dominates the mma loop:
//
//     add.u64  %rd26, %SPL, 0;     // accumulator tile base
//     add.s64  %rd7,  %rd26, 32;   // store base, captured here
//     $L__BB0_7: ... add.s64 %rd26, %rd26, 64;   // loop redefines %rd26
//     $L__BB0_11: add.s64 %rd22, %rd7, %rd29;    // reads the pre-loop value
//
// — but 12.0 allocates the store base onto UR5, the mma loop's own induction
// uniform (`UIADD3 UR5, UR5, 0x40`), so the store reads 16*64 = 1024 bytes past
// the accumulator: it writes back the B and A tiles instead of the product.
// 12.9 and 13.3 both use a separate uniform and are correct.
//
// It assembles with NO error and NO warning, so the only symptom was a device
// test returning structured garbage. That went unexplained for five nightlies
// (2026-09-13 .. 2026-09-17) because the two device legs disagreed: the Windows
// runner found CUDA 12.9 on PATH and passed, while the WSL runner — whose
// service environment has no CUDA_PATH and no /usr/local/cuda/bin — fell
// through to /usr/bin/ptxas 12.0 and failed.
//
// So the floor below is not a style preference. Assembling with a ptxas known
// to produce wrong device results must be a loud failure, never a wrong answer.
//

#include "gtest/gtest.h"
#include "../PortableEnv.h"   // portable setenv/unsetenv on MinGW

#include "cajeta/xpu/nvidia/NvptxBackend.h"

#include <filesystem>
#include <fstream>
#include <ostream>
#include <string>

using cajeta::xpu::nvidia::PtxasVersion;
using cajeta::xpu::nvidia::parsePtxasVersion;
using cajeta::xpu::nvidia::ptxasVersionSupported;
using cajeta::xpu::nvidia::kMinPtxasVersion;
using cajeta::xpu::nvidia::findPtxas;

// Without this gtest byte-dumps the struct, and "12.0 vs 12.1" is the whole
// point of every assertion below.
namespace cajeta { namespace xpu { namespace nvidia {
void PrintTo(const PtxasVersion& v, std::ostream* os) {
    if (v.unknown()) *os << "CUDA <unknown>";
    else *os << "CUDA " << v.major << "." << v.minor;
}
}}}

namespace {

// Verbatim `ptxas --version` output from the three assemblers this bug was
// measured against, so the parser is pinned to real text rather than a guess.
const char* kText120 =
    "ptxas: NVIDIA (R) Ptx optimizing assembler\n"
    "Copyright (c) 2005-2022 NVIDIA Corporation\n"
    "Built on Fri_Jan__6_16:43:29_PST_2023\n"
    "Cuda compilation tools, release 12.0, V12.0.140\n"
    "Build cuda_12.0.r12.0/compiler.32267302_0\n";

const char* kText129 =
    "ptxas: NVIDIA (R) Ptx optimizing assembler\n"
    "Copyright (c) 2005-2025 NVIDIA Corporation\n"
    "Built on Wed_Apr_09_19:29:17_PDT_2025\n"
    "Cuda compilation tools, release 12.9, V12.9.86\n"
    "Build cuda_12.9.r12.9/compiler.36037853_0\n";

const char* kText133 =
    "ptxas: NVIDIA (R) Ptx optimizing assembler\n"
    "Copyright (c) 2005-2026 NVIDIA Corporation\n"
    "Built on Tue_Jun_09_02:43:40_PM_PDT_2026\n"
    "Cuda compilation tools, release 13.3, V13.3.73\n"
    "Build cuda_13.3.r13.3/compiler.38244171_0\n";

} // namespace

// ---- the parser -------------------------------------------------------

TEST(NvptxPtxasVersionTests, parsesTheReleaseLineOfEachRealAssembler) {
    EXPECT_EQ(parsePtxasVersion(kText120), (PtxasVersion{12, 0}));
    EXPECT_EQ(parsePtxasVersion(kText129), (PtxasVersion{12, 9}));
    EXPECT_EQ(parsePtxasVersion(kText133), (PtxasVersion{13, 3}));
}

// An unreadable version is {0,0} — DISTINCT from "old", because the two get
// different treatment below (unknown proceeds, old does not).
TEST(NvptxPtxasVersionTests, unparseableVersionTextIsUnknownNotZeroPointZero) {
    EXPECT_EQ(parsePtxasVersion(""), (PtxasVersion{0, 0}));
    EXPECT_EQ(parsePtxasVersion("not a ptxas at all\n"), (PtxasVersion{0, 0}));
    // A truncated release line carries no minor — still unusable, so unknown.
    EXPECT_EQ(parsePtxasVersion("Cuda compilation tools, release \n"),
              (PtxasVersion{0, 0}));
    EXPECT_FALSE(parsePtxasVersion(kText133).unknown());
    EXPECT_TRUE(parsePtxasVersion("").unknown());
}

// A minor that is two digits must not compare as a smaller number than a
// one-digit minor — 12.10 is NEWER than 12.9, not older.
TEST(NvptxPtxasVersionTests, comparesMinorsNumericallyNotLexically) {
    EXPECT_LT((PtxasVersion{12, 9}), (PtxasVersion{12, 10}));
    EXPECT_LT((PtxasVersion{12, 9}), (PtxasVersion{13, 0}));
    EXPECT_LT((PtxasVersion{9, 0}), (PtxasVersion{12, 0}));
}

// ---- the gate: it must FIRE on the bad one ----------------------------

TEST(NvptxPtxasVersionTests, rejectsTheMiscompilingCuda120) {
    EXPECT_FALSE(ptxasVersionSupported(parsePtxasVersion(kText120)))
        << "CUDA 12.0 ptxas miscompiles the portable CooperativeMatrix store "
           "base onto the mma loop's induction uniform";
    EXPECT_FALSE(ptxasVersionSupported(PtxasVersion{11, 8}));
    EXPECT_FALSE(ptxasVersionSupported(PtxasVersion{12, 0}));
}

// ---- and it must NOT fire on the good ones ----------------------------

TEST(NvptxPtxasVersionTests, acceptsEveryAssemblerMeasuredCorrect) {
    EXPECT_TRUE(ptxasVersionSupported(parsePtxasVersion(kText129)))
        << "12.9 is what the Windows device leg uses, and it passes";
    EXPECT_TRUE(ptxasVersionSupported(parsePtxasVersion(kText133)))
        << "13.3 is what /usr/local/cuda provides, and it passes";
    EXPECT_TRUE(ptxasVersionSupported(kMinPtxasVersion));
}

// An unknown version PROCEEDS. Refusing to assemble because `--version` could
// not be read would turn a working toolchain into a hard failure, which is a
// worse trade than the bug this gate exists for: ptxas itself still errors on
// PTX it cannot handle, so the wrong-answer path stays closed either way.
TEST(NvptxPtxasVersionTests, unknownVersionIsAllowedThrough) {
    EXPECT_TRUE(ptxasVersionSupported(PtxasVersion{0, 0}));
}

TEST(NvptxPtxasVersionTests, theFloorIsAboveTheKnownBadRelease) {
    EXPECT_LT((PtxasVersion{12, 0}), kMinPtxasVersion);
}

// ---- selection: CUDA_PATH wins over PATH ------------------------------

// The whole WSL failure was findPtxas() falling through to PATH because
// CUDA_PATH was unset. Pin that CUDA_PATH is consulted first, so a box with a
// stale /usr/bin/ptxas can be steered by setting it.
TEST(NvptxPtxasVersionTests, cudaPathIsPreferredOverPath) {
    namespace fs = std::filesystem;
    fs::path root = fs::temp_directory_path()
                  / ("cajeta_ptxas_pick_" + std::to_string(cajeta_getpid()));
    fs::create_directories(root / "bin");
    // findPtxas probes "<CUDA_PATH>/bin/ptxas.exe" then "<CUDA_PATH>/bin/ptxas"
    // by EXISTENCE, so a plain file stands in for the assembler on every host.
    fs::path fake = root / "bin" / "ptxas";
    std::ofstream(fake) << "#!/bin/sh\nexit 0\n";

    const char* prev = std::getenv("CUDA_PATH");
    std::string saved = prev ? prev : "";
    setenv("CUDA_PATH", root.string().c_str(), 1);
    std::string picked = findPtxas();
    if (saved.empty()) unsetenv("CUDA_PATH");
    else setenv("CUDA_PATH", saved.c_str(), 1);

    // Canonicalized on both sides: findPtxas joins with '/', which on Windows
    // leaves a mixed-separator path that compares unequal to the native form.
    std::error_code pec;
    EXPECT_EQ(fs::weakly_canonical(fs::path(picked), pec),
              fs::weakly_canonical(fake, pec))
        << "CUDA_PATH must win, so a box whose PATH holds a stale ptxas can be "
           "steered without reordering PATH";
    std::error_code ec;
    fs::remove_all(root, ec);
}
