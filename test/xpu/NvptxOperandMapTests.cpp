//
// The lane-to-element maps of NVIDIA's m16n16k16 OPERAND fragments, measured on the
// device (coop-converting-store Unit 5, spec §2.5). PTX leaves a wmma fragment's
// layout unspecified, so the unaligned-load fallback gathers each lane's elements
// through a map this test measures rather than one assumed from the mma docs.
//
// The probe is hand-written PTX: wmma.load.{a,b} of an index tile, whose cell at
// memory offset i holds the value i, then every lane writes its registers out. Each
// register element then names the memory offset it read, at stride 16, so the map
// is layout-agnostic: element k of lane L reads (v / 16) * stride + v % 16.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/nvidia/CudaDriver.h"
#include "cajeta/xpu/nvidia/NvptxBackend.h"
#include "cajeta/xpu/nvidia/NvptxKernelLowering.h"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace cajeta::xpu::nvidia;

namespace {

struct Variant {
    const char* use;    // "a" or "b"
    const char* type;   // "f16", "bf16", "u8"
    const char* layout; // "row" or "col"
    unsigned regs;      // b32 registers in the fragment
    unsigned perReg;    // elements a register packs
};

const Variant kVariants[] = {
    {"a", "f16", "row", 8, 2}, {"a", "f16", "col", 8, 2},
    {"b", "f16", "row", 8, 2}, {"b", "f16", "col", 8, 2},
    {"a", "bf16", "row", 4, 2}, {"a", "bf16", "col", 4, 2},
    {"b", "bf16", "row", 4, 2}, {"b", "bf16", "col", 4, 2},
    {"a", "u8", "row", 2, 4}, {"a", "u8", "col", 2, 4},
    {"b", "u8", "row", 2, 4}, {"b", "u8", "col", 2, 4},
};

std::string probePtx(const Variant& v) {
    std::string regs;
    for (unsigned r = 0; r < v.regs; ++r) regs += (r ? ", %r" : "%r") + std::to_string(r);
    std::string ptx =
        ".version 7.8\n.target sm_89\n.address_size 64\n"
        ".visible .entry probe(.param .u64 src, .param .u64 out) {\n"
        "  .reg .b64 %rd<6>;\n  .reg .b32 %r<16>;\n"
        "  ld.param.u64 %rd1, [src];\n  ld.param.u64 %rd2, [out];\n"
        "  cvta.to.global.u64 %rd1, %rd1;\n  cvta.to.global.u64 %rd2, %rd2;\n"
        "  wmma.load." + std::string(v.use) + ".sync.aligned." + v.layout +
        ".m16n16k16.global." + v.type + " {" + regs + "}, [%rd1], 16;\n"
        "  mov.u32 %r15, %tid.x;\n"
        "  mul.wide.u32 %rd3, %r15, " + std::to_string(4 * v.regs) + ";\n"
        "  add.s64 %rd4, %rd2, %rd3;\n";
    for (unsigned r = 0; r < v.regs; ++r)
        ptx += "  st.global.b32 [%rd4+" + std::to_string(4 * r) + "], %r" + std::to_string(r) +
               ";\n";
    ptx += "  ret;\n}\n";
    return ptx;
}

// The index tile: cell i holds i, in the variant's element encoding.
std::vector<uint8_t> indexTile(const Variant& v) {
    std::vector<uint8_t> bytes;
    for (unsigned i = 0; i < 256; ++i) {
        if (std::string(v.type) == "u8") {
            bytes.push_back((uint8_t) i);
            continue;
        }
        float f = (float) i;
        uint32_t fb;
        std::memcpy(&fb, &f, 4);
        uint16_t h;
        if (std::string(v.type) == "bf16") {
            h = (uint16_t) (fb >> 16);  // exact for integers up to 256
        } else {
            // f16 of a small integer: exponent and the top ten mantissa bits.
            if (i == 0) h = 0;
            else {
                int e = (int) ((fb >> 23) & 0xff) - 127 + 15;
                h = (uint16_t) ((e << 10) | ((fb >> 13) & 0x3ff));
            }
        }
        bytes.push_back((uint8_t) (h & 0xff));
        bytes.push_back((uint8_t) (h >> 8));
    }
    return bytes;
}

// Element k of each lane: the memory offset it read, decoded from the register image.
std::vector<int> decode(const Variant& v, const std::vector<uint32_t>& out) {
    std::vector<int> map;
    for (unsigned lane = 0; lane < 32; ++lane)
        for (unsigned r = 0; r < v.regs; ++r) {
            uint32_t w = out[lane * v.regs + r];
            for (unsigned p = 0; p < v.perReg; ++p) {
                if (std::string(v.type) == "u8") {
                    map.push_back((int) ((w >> (8 * p)) & 0xff));
                    continue;
                }
                uint16_t h = (uint16_t) (w >> (16 * p));
                float f;
                if (std::string(v.type) == "bf16") {
                    uint32_t fb = (uint32_t) h << 16;
                    std::memcpy(&f, &fb, 4);
                } else if (h == 0) {
                    f = 0;
                } else {
                    uint32_t fb = ((uint32_t) (((h >> 10) & 0x1f) - 15 + 127) << 23) |
                                  ((uint32_t) (h & 0x3ff) << 13);
                    std::memcpy(&f, &fb, 4);
                }
                map.push_back((int) f);
            }
        }
    return map;
}

std::vector<int> measure(CudaDriver& cuda, const Variant& v) {
    std::vector<uint8_t> cubin = assembleCubin(probePtx(v), "sm_89");
    if (cubin.empty()) return {};
    CudaModule mod = cuda.loadModule(cubin.data(), cubin.size());
    CudaFunction fn = cuda.getFunction(mod, "probe");
    std::vector<uint8_t> tile = indexTile(v);
    std::vector<uint32_t> out(32 * v.regs, 0xffffffffu);
    CudaDevicePtr dSrc = cuda.alloc(tile.size());
    CudaDevicePtr dOut = cuda.alloc(out.size() * 4);
    cuda.memcpyHtoD(dSrc, tile.data(), tile.size());
    cuda.memcpyHtoD(dOut, out.data(), out.size() * 4);
    void* params[] = {&dSrc, &dOut};
    bool ok = cuda.launch(fn, 1, 32, params) && cuda.synchronize();
    cuda.memcpyDtoH(out.data(), dOut, out.size() * 4);
    cuda.free(dSrc);
    cuda.free(dOut);
    return ok ? decode(v, out) : std::vector<int>{};
}

} // namespace

// Every variant's fragment covers its 256 cells, each the number of times the wmma
// layout replicates it, and the measured map is printed for the lowering's table.
TEST(NvptxOperandMapDeviceTests, everyOperandFragmentMapIsMeasured) {
    if (!CudaDriver::available()) GTEST_SKIP() << "no CUDA device/driver available";
    CudaDriver cuda;
    ASSERT_TRUE(cuda.init());
    for (const Variant& v : kVariants) {
        std::vector<int> map = measure(cuda, v);
        const std::string name = std::string(v.use) + "." + v.type + "." + v.layout;
        ASSERT_EQ(map.size(), 32u * v.regs * v.perReg) << name << " did not run";
        std::vector<int> seen(256, 0);
        for (int m : map) {
            ASSERT_GE(m, 0) << name;
            ASSERT_LT(m, 256) << name;
            ++seen[m];
        }
        const int copies = (int) map.size() / 256;
        for (int i = 0; i < 256; ++i) EXPECT_EQ(seen[i], copies) << name << " cell " << i;
        std::string line = name + ":";
        for (size_t i = 0; i < map.size(); ++i)
            line += (i % (v.regs * v.perReg) ? "," : " | ") + std::to_string(map[i]);
        std::fprintf(stderr, "%s\n", line.c_str());
    }
}

// The grid that sets the guard (measured 2026-11-18 on the 4090, one process a cell):
// a lane's contiguous run must be aligned, 4 bytes for f16 and bf16 fragments and the
// row-major u8 A operand, 8 for the f32 accumulator, and the stride in bytes a
// multiple of the same. A misaligned tile faults; a stride off that multiple puts
// cells in the wrong place with no error. The row-major u8 B operand loads at any
// placement.
namespace {
std::string gridLoadPtx(const Variant& v, unsigned byteOff, unsigned ldm) {
    std::string ptx = probePtx(v);
    const std::string from = "[%rd1], 16;";
    ptx.replace(ptx.find(from), from.size(),
                "[%rd1+" + std::to_string(byteOff) + "], " + std::to_string(ldm) + ";");
    return ptx;
}
// Load the f32 (or f16) accumulator from an aligned index tile at ldm 16, store it
// at (byteOff, ldm) into a sentinel buffer.
std::string storePtx(bool f16, unsigned byteOff, unsigned ldm) {
    const unsigned n = f16 ? 4 : 8;
    std::string regs;
    for (unsigned r = 0; r < n; ++r) regs += (r ? ", %r" : "%r") + std::to_string(r);
    const std::string t = f16 ? "f16" : "f32";
    return ".version 7.8\n.target sm_89\n.address_size 64\n"
           ".visible .entry probe(.param .u64 src, .param .u64 out) {\n"
           "  .reg .b64 %rd<6>;\n  .reg .b32 %r<16>;\n"
           "  ld.param.u64 %rd1, [src];\n  ld.param.u64 %rd2, [out];\n"
           "  cvta.to.global.u64 %rd1, %rd1;\n  cvta.to.global.u64 %rd2, %rd2;\n"
           "  wmma.load.c.sync.aligned.row.m16n16k16.global." + t + " {" + regs + "}, [%rd1], 16;\n"
           "  wmma.store.d.sync.aligned.row.m16n16k16.global." + t + " [%rd2+" +
           std::to_string(byteOff) + "], {" + regs + "}, " + std::to_string(ldm) + ";\n"
           "  ret;\n}\n";
}
bool runOnce(CudaDriver& cuda, const std::string& ptx, const std::vector<uint8_t>& src,
             std::vector<uint8_t>& out) {
    std::vector<uint8_t> cubin = assembleCubin(ptx, "sm_89");
    if (cubin.empty()) return false;
    CudaModule mod = cuda.loadModule(cubin.data(), cubin.size());
    CudaFunction fn = cuda.getFunction(mod, "probe");
    CudaDevicePtr dSrc = cuda.alloc(src.size());
    CudaDevicePtr dOut = cuda.alloc(out.size());
    cuda.memcpyHtoD(dSrc, src.data(), src.size());
    cuda.memcpyHtoD(dOut, out.data(), out.size());
    void* params[] = {&dSrc, &dOut};
    bool ok = cuda.launch(fn, 1, 32, params) && cuda.synchronize();
    cuda.memcpyDtoH(out.data(), dOut, out.size());
    cuda.free(dSrc);
    cuda.free(dOut);
    return ok;
}
}

// One cell of the grid per process, since a misaligned wmma faults and poisons the
// context: NVPTX_WMMA_CELL="<a.f16|b.bf16|...|d.f32|d.f16>,<element offset>,<ldm>".
// Prints "cell ok", "cell N bad" or "cell fault". Driven by a shell loop; skipped
// without the variable.
TEST(NvptxOperandMapDeviceTests, wmmaAlignmentCell) {
    const char* spec = std::getenv("NVPTX_WMMA_CELL");
    if (!spec) GTEST_SKIP() << "set NVPTX_WMMA_CELL to probe one cell";
    if (!CudaDriver::available()) GTEST_SKIP() << "no CUDA device/driver available";
    std::string cellName(spec);
    std::string var = cellName.substr(0, cellName.find(','));
    std::string rest = cellName.substr(cellName.find(',') + 1);
    unsigned eo = (unsigned) std::stoul(rest.substr(0, rest.find(',')));
    unsigned ldm = (unsigned) std::stoul(rest.substr(rest.find(',') + 1));
    CudaDriver cuda;
    ASSERT_TRUE(cuda.init());
    std::string verdict;
    if (var[0] == 'd') {
        const bool f16 = var == "d.f16";
        const unsigned eb = f16 ? 2 : 4;
        std::vector<uint8_t> src;
        Variant hv{"a", "f16", "row", 8, 2};
        std::vector<uint8_t> enc = indexTile(hv);
        for (unsigned i = 0; i < 256; ++i) {
            float f = (float) i;
            if (!f16) {
                uint8_t b[4];
                std::memcpy(b, &f, 4);
                src.insert(src.end(), b, b + 4);
            } else {
                src.push_back(enc[2 * i]);
                src.push_back(enc[2 * i + 1]);
            }
        }
        std::vector<uint8_t> out(4096 * eb, 0xff);
        bool ok = runOnce(cuda, storePtx(f16, eo * eb, ldm), src, out);
        unsigned bad = 0;
        for (unsigned r = 0; ok && r < 16; ++r)
            for (unsigned c = 0; c < 16; ++c) {
                unsigned at = (eo + r * ldm + c) * eb;
                float g;
                if (!f16) std::memcpy(&g, &out[at], 4);
                else {
                    uint16_t h = (uint16_t) (out[at] | (out[at + 1] << 8));
                    if (h == 0) g = 0;
                    else {
                        uint32_t fb = ((uint32_t) (((h >> 10) & 0x1f) - 15 + 127) << 23) |
                                      ((uint32_t) (h & 0x3ff) << 13);
                        std::memcpy(&g, &fb, 4);
                    }
                }
                if (g != (float) (r * 16 + c)) ++bad;
            }
        verdict = !ok ? "fault" : bad ? std::to_string(bad) + " bad" : "ok";
    } else {
        const Variant* v = nullptr;
        for (const Variant& w : kVariants)
            if (var == std::string(w.use) + "." + w.type && std::string(w.layout) == "row") v = &w;
        ASSERT_TRUE(v) << var;
        std::vector<int> base = measure(cuda, *v);
        ASSERT_FALSE(base.empty());
        const unsigned eb = std::string(v->type) == "u8" ? 1 : 2;
        std::vector<uint8_t> enc = indexTile(*v), tile;
        for (unsigned i = 0; i < 2048; ++i) {
            unsigned val = i % 256;
            if (eb == 1) tile.push_back((uint8_t) val);
            else { tile.push_back(enc[2 * val]); tile.push_back(enc[2 * val + 1]); }
        }
        std::vector<uint8_t> out(32 * v->regs * 4, 0xff);
        bool ok = runOnce(cuda, gridLoadPtx(*v, eo * eb, ldm), tile, out);
        std::vector<uint32_t> words(32 * v->regs);
        std::memcpy(words.data(), out.data(), out.size());
        std::vector<int> got = ok ? decode(*v, words) : std::vector<int>{};
        unsigned bad = 0;
        for (size_t i = 0; i < got.size(); ++i) {
            unsigned mem = (unsigned) (base[i] / 16) * ldm + (unsigned) (base[i] % 16) + eo;
            if ((unsigned) got[i] != mem % 256) ++bad;
        }
        verdict = !ok ? "fault" : bad ? std::to_string(bad) + " bad" : "ok";
    }
    std::fprintf(stderr, "WMMACELL %s %s\n", spec, verdict.c_str());
}
