//
// NvptxCoopFromWordsTests — CooperativeMatrix.fromWords on NVIDIA
// (xpu-kernel-adaptor plan, 4A.2.4).
//
// The verb hands the backend four i32 words. On AMD WMMA those words ARE this
// lane's fragment: 16 bytes per lane, lane L owning column L%16, the same 256
// bytes held twice across the two half-waves of a wave32. NVIDIA's m16n16k16
// s8 operand fragment is {i32 x 2} — eight bytes per lane, 32 lanes, 256 bytes
// with no duplication and a different partition — so a lane does not hold the
// bytes its own slots need and the data has to cross lanes. NVPTX therefore
// stages the words into shared memory in the layout they logically describe
// and re-loads through the ordinary wmma load, which redistributes in hardware.
//
// WHAT THIS FILE PINS. That staging rests on one assumption, and it was
// inherited from AMD rather than measured: that the lane's 16 bytes are the
// sixteen K values of column lane%16, so element (k,n) belongs at n*16 + k
// (column-major, stride 16). If that is wrong the kernels still lower, still
// run, and quietly compute a permuted product — which is exactly what the llm
// suite showed: `q4kWmmaIdMw8Kernel` agreed with its reference while
// `q4kWmmaIdMwKernel` and `q6kWmmaIdMwKernel` did not.
//
// HOW IT IS MEASURED. Recover the matrix instead of arguing about it. With A
// the 16x16 identity and C zero, D = A*B is B itself, so storing the int32
// accumulator reads the operand back element by element. Each byte is given a
// value that identifies its own (k, n), so a wrong mapping prints as a
// readable permutation rather than a wall of numbers.
//
// The value chosen is 1 + k + 16*n reduced into int8 range via a stride that
// keeps every cell distinct within a column, so both coordinates are
// recoverable from the number that comes back.
//

#include <gtest/gtest.h>

#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"
#include "../PortableEnv.h"

#include "cajeta/xpu/nvidia/CudaDriver.h"
#include "cajeta/xpu/nvidia/NvptxBackend.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace cajeta::xpu::probe;

namespace {

constexpr unsigned N = 16;
constexpr unsigned TILE = N * N;

// `words` holds 4 i32 per lane, exactly what the kernel would have computed.
// A is the identity so the product returns B unchanged.
//
// @Occupancy is required by the nvptx staging (it sizes the per-wave buffer
// from the block bound rather than inventing one), and 32 is the literal block
// this test launches.
const char* kSource =
    "package test;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    @Occupancy(maxThreads = 32)\n"
    "    public static void fw(KernelBuffer<int8> ident,\n"
    "            KernelBuffer<int32> words, KernelBuffer<int32> out) {\n"
    "        uint32 lane = KernelThread.x();\n"
    "        CooperativeMatrix<int8,16,16,0> ma;\n"
    "        ma.load(ident, 0, 0, 16);\n"
    "        CooperativeMatrix<int8,16,16,1> mb;\n"
    "        mb.fromWords(words[(int64) lane * 4L],\n"
    "                     words[(int64) lane * 4L + 1L],\n"
    "                     words[(int64) lane * 4L + 2L],\n"
    "                     words[(int64) lane * 4L + 3L]);\n"
    "        CooperativeMatrix<int32,16,16,2> mc;\n"
    "        mc.splat(0);\n"
    "        mc.mma(ma, mb);\n"
    "        mc.store(out, 0, 0, 16);\n"
    "    }\n"
    "    public static int32 run() { return 1; }\n"
    "}\n";

// The byte a lane contributes for (k, n): distinct per cell and inside int8.
int8_t cellValue(unsigned k, unsigned n) {
    return (int8_t) (1 + (int) k + 16 * (int) (n % 7));
}

bool runFromWords(std::vector<int32_t>* out, std::string* why) {
    const Lowered l = lowerForNvptx(kSource, "fw", "test.M", "sm_89", "coopfw");
    if (!l.ok) { *why = l.why; return false; }
    std::vector<uint8_t> cubin =
        cajeta::xpu::nvidia::assembleCubin(l.ptx, "sm_89");
    if (cubin.empty()) { *why = "ptxas rejected the PTX"; return false; }

    cajeta::xpu::nvidia::CudaDriver cuda;
    if (!cuda.init()) { *why = "cuda init failed"; return false; }
    auto mod = cuda.loadModule(cubin.data(), cubin.size());
    if (!mod) { *why = "loadModule failed"; return false; }
    auto fn = cuda.getFunction(mod, "fw");
    if (!fn) { *why = "getFunction failed"; return false; }

    // A = identity, row-major.
    std::vector<int8_t> ident(TILE, 0);
    for (unsigned i = 0; i < N; ++i) ident[i * N + i] = 1;

    // Each lane's four words = the sixteen K bytes of column lane%16, which is
    // the contract the nvptx staging assumes. Both half-waves carry the same
    // column, as AMD's fragment does.
    std::vector<int32_t> words(32 * 4, 0);
    for (unsigned lane = 0; lane < 32; ++lane) {
        const unsigned n = lane % N;
        for (unsigned w = 0; w < 4; ++w) {
            uint32_t packed = 0;
            for (unsigned byte = 0; byte < 4; ++byte) {
                const unsigned k = w * 4 + byte;
                packed |= ((uint32_t) (uint8_t) cellValue(k, n)) << (8 * byte);
            }
            words[lane * 4 + w] = (int32_t) packed;
        }
    }

    auto dI = cuda.alloc(TILE);
    auto dW = cuda.alloc(words.size() * sizeof(int32_t));
    auto dO = cuda.alloc(TILE * sizeof(int32_t));
    cuda.memcpyHtoD(dI, (void*) ident.data(), TILE);
    cuda.memcpyHtoD(dW, (void*) words.data(), words.size() * sizeof(int32_t));
    void* params[] = { &dI, &dW, &dO };
    bool ok = cuda.launch(fn, /*grid=*/1, /*block=*/32, params)
              && cuda.synchronize();
    out->assign(TILE, INT32_MIN);
    if (ok) ok = cuda.memcpyDtoH(out->data(), dO, TILE * sizeof(int32_t));
    cuda.free(dI); cuda.free(dW); cuda.free(dO);
    if (!ok) { *why = "launch or copy-back failed"; return false; }
    return true;
}

// EIGHT WAVES, which is the gap between the single-warp test above and the
// kernels that failed. Those run 8 waves per block and each stages a
// different operand at the same moment, so a wrong wave slice shows up here
// and nowhere else. Every wave is given its own value range, so a wave
// reading its neighbour's columns is visible as the neighbour's numbers.
const char* kMultiWaveSource =
    "package test;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    @Occupancy(maxThreads = 256)\n"
    "    public static void fwMw(KernelBuffer<int8> ident,\n"
    "            KernelBuffer<int32> words, KernelBuffer<int32> out) {\n"
    "        uint32 tid = KernelThread.x();\n"
    "        uint32 wid = tid / 32;\n"
    "        CooperativeMatrix<int8,16,16,0> ma;\n"
    "        ma.load(ident, 0, 0, 16);\n"
    "        CooperativeMatrix<int8,16,16,1> mb;\n"
    "        mb.fromWords(words[(int64) tid * 4L],\n"
    "                     words[(int64) tid * 4L + 1L],\n"
    "                     words[(int64) tid * 4L + 2L],\n"
    "                     words[(int64) tid * 4L + 3L]);\n"
    "        CooperativeMatrix<int32,16,16,2> mc;\n"
    "        mc.splat(0);\n"
    "        mc.mma(ma, mb);\n"
    "        mc.store(out, wid * 256, 0, 16);\n"
    "    }\n"
    "    public static int32 run() { return 1; }\n"
    "}\n";

// Wave w contributes 1 + k + 16*(n%7) + 100*w, so a slice collision reads as
// another wave's hundreds digit.
int8_t cellValueW(unsigned k, unsigned n, unsigned w) {
    return (int8_t) (1 + (int) k + 16 * (int) (n % 7) + 100 * (int) w);
}

} // namespace

TEST(NvptxCoopFromWordsTests, eachWaveStagesIntoItsOwnSlice) {
    CAJETA_SKIP_IF_NO_CUDA();

    const Lowered l =
        lowerForNvptx(kMultiWaveSource, "fwMw", "test.M", "sm_89", "coopfwmw");
    ASSERT_TRUE(l.ok) << l.why;
    std::vector<uint8_t> cubin =
        cajeta::xpu::nvidia::assembleCubin(l.ptx, "sm_89");
    ASSERT_FALSE(cubin.empty()) << "ptxas rejected the PTX";

    cajeta::xpu::nvidia::CudaDriver cuda;
    ASSERT_TRUE(cuda.init());
    auto mod = cuda.loadModule(cubin.data(), cubin.size());
    ASSERT_TRUE(mod);
    auto fn = cuda.getFunction(mod, "fwMw");
    ASSERT_TRUE(fn);

    constexpr unsigned WAVES = 8;
    std::vector<int8_t> ident(TILE, 0);
    for (unsigned i = 0; i < N; ++i) ident[i * N + i] = 1;

    std::vector<int32_t> words(WAVES * 32 * 4, 0);
    for (unsigned t = 0; t < WAVES * 32; ++t) {
        const unsigned w = t / 32, n = (t % 32) % N;
        for (unsigned q = 0; q < 4; ++q) {
            uint32_t packed = 0;
            for (unsigned byte = 0; byte < 4; ++byte)
                packed |= ((uint32_t) (uint8_t) cellValueW(q * 4 + byte, n, w))
                          << (8 * byte);
            words[t * 4 + q] = (int32_t) packed;
        }
    }

    auto dI = cuda.alloc(TILE);
    auto dW = cuda.alloc(words.size() * sizeof(int32_t));
    auto dO = cuda.alloc(WAVES * TILE * sizeof(int32_t));
    cuda.memcpyHtoD(dI, (void*) ident.data(), TILE);
    cuda.memcpyHtoD(dW, (void*) words.data(), words.size() * sizeof(int32_t));
    void* params[] = { &dI, &dW, &dO };
    ASSERT_TRUE(cuda.launch(fn, /*grid=*/1, /*block=*/WAVES * 32, params));
    ASSERT_TRUE(cuda.synchronize());
    std::vector<int32_t> got(WAVES * TILE, INT32_MIN);
    ASSERT_TRUE(cuda.memcpyDtoH(got.data(), dO,
                                WAVES * TILE * sizeof(int32_t)));
    cuda.free(dI); cuda.free(dW); cuda.free(dO);

    std::size_t bad = 0;
    std::string detail;
    for (unsigned w = 0; w < WAVES; ++w)
        for (unsigned k = 0; k < N; ++k)
            for (unsigned n = 0; n < N; ++n) {
                const int want = (int) cellValueW(k, n, w);
                const int have = got[w * TILE + k * N + n];
                if (have == want) continue;
                if (bad < 8)
                    detail += "    wave " + std::to_string(w) + " (k=" +
                              std::to_string(k) + ",n=" + std::to_string(n) +
                              ") want " + std::to_string(want) + " got " +
                              std::to_string(have) + "\n";
                ++bad;
            }
    EXPECT_EQ(bad, 0u)
        << bad << " of " << (WAVES * TILE) << " cells wrong across 8 waves — "
           "the per-wave staging slices overlap:\n" << detail;
}


// The contract, measured. Identity times B is B, so out[k*16 + n] must be the
// byte the lane owning column n contributed for row k. A wrong staging layout
// prints which (k, n) actually landed where.
TEST(NvptxCoopFromWordsTests, theStagedOperandComesBackAtItsOwnRowAndColumn) {
    CAJETA_SKIP_IF_NO_CUDA();

    std::string why;
    std::vector<int32_t> got;
    ASSERT_TRUE(runFromWords(&got, &why)) << why;

    std::size_t bad = 0;
    std::string detail;
    for (unsigned k = 0; k < N; ++k) {
        for (unsigned n = 0; n < N; ++n) {
            const int want = (int) cellValue(k, n);
            const int have = got[k * N + n];
            if (have == want) continue;
            if (bad < 8) {
                detail += "    (k=" + std::to_string(k) + ",n=" +
                          std::to_string(n) + ") want " +
                          std::to_string(want) + " got " +
                          std::to_string(have) + "\n";
            }
            ++bad;
        }
    }
    EXPECT_EQ(bad, 0u)
        << bad << " of " << TILE << " cells are wrong — the nvptx staging "
           "places fromWords' bytes at the wrong (row, column). The verb's "
           "AMD contract is that a lane's 16 bytes are the K values of column "
           "lane%16, staged at n*16 + k:\n" << detail;
}

// The staging needs the block bound to size its per-wave buffer, and inventing
// one would silently let a wave write over its neighbour's columns. A kernel
// that does not declare it must be refused BY NAME, not guessed at.
TEST(NvptxCoopFromWordsTests, anUndeclaredBlockBoundIsRefusedByName) {
    static const char* kNoOccupancy =
        "package test;\n"
        "import cajeta.xpu.CooperativeMatrix;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void fwNo(KernelBuffer<int32> words,\n"
        "            KernelBuffer<int32> out) {\n"
        "        CooperativeMatrix<int8,16,16,1> mb;\n"
        "        mb.fromWords(words[0], words[1], words[2], words[3]);\n"
        "        CooperativeMatrix<int8,16,16,0> ma;\n"
        "        CooperativeMatrix<int32,16,16,2> mc;\n"
        "        mc.splat(0);\n"
        "        mc.mma(ma, mb);\n"
        "        mc.store(out, 0, 0, 16);\n"
        "    }\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    const Lowered l =
        lowerForNvptx(kNoOccupancy, "fwNo", "test.M", "sm_89", "coopfwno");
    EXPECT_FALSE(l.ok)
        << "a kernel with no @Occupancy bound lowered anyway, so the staging "
           "buffer was sized from a guess";
    if (!l.ok)
        EXPECT_NE(l.why.find("Occupancy"), std::string::npos)
            << "the refusal must name the remedy. Got: " << l.why;
}
