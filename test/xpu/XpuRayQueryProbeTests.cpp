//
// cajeta-gpu Part C bring-up — Vulkan ray-query SPIR-V EXPRESSIBILITY probe.
//
// Closes the #1 open risk from the RT-as-compute research pass: every published
// result is OptiX / CUDA RT-core based, leaving "does the SAME spatial-index
// encoding work through Vulkan ray query, in an ORDINARY compute shader, as valid
// SPIR-V?" unconfirmed. This probe answers it on our actual target env — and, just
// as importantly, produces the concrete SPIR-V the future LLVM lowering
// (cajeta-gpu Part C / Stage C3.3, `SPV_KHR_ray_query`) must learn to emit.
//
// What it does NOT do: run on-device (no acceleration-structure build / dispatch —
// that is C3.3 runtime work in the device layer) and NOT lower from Cajeta source
// (the LLVM backend can't emit ray query yet — that IS C3.3). It proves the
// *shader-side encoding is expressible and Vulkan-valid*, decoupled from both.
//
// Mechanism: author the canonical "BVH-as-spatial-index with degenerate rays"
// pattern (RTNN, PPoPP'22) as a GLSL compute shader using GL_EXT_ray_query, compile
// with glslangValidator → SPIR-V for vulkan1.3, prove it with spirv-val, and assert
// the disassembly carries the ray-query opcodes UNDER A GLCompute ENTRY POINT (i.e.
// no raygen/closest-hit/SBT pipeline needed — the research's refuted "full pipeline
// mandatory" claim, made concrete). GPU-free: glslang / spirv-val / spirv-dis are
// host tools; the test SKIPs cleanly if glslang is absent.
//

#include "gtest/gtest.h"

#include "llvm/Support/Program.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <string>

namespace {

// The canonical spatial-index query as a COMPUTE shader: a degenerate (zero-length)
// ray at each query point, traversing a BVH whose leaves are per-point AABBs; each
// candidate AABB is a neighbor to gather/count. No geometric hit is committed — the
// candidate visit itself is the spatial-query payload. This is the encoding Caramelo's
// SpatialIndex hides (see cajeta-caramelo-plan.md P1).
constexpr const char* kRayQueryComputeGlsl = R"GLSL(
#version 460
#extension GL_EXT_ray_query : require

layout(local_size_x = 64) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT scene;
layout(set = 0, binding = 1) readonly  buffer Queries { vec4 q[]; };   // xyz = origin, w = radius
layout(set = 0, binding = 2) writeonly buffer Counts  { uint counts[]; };

void main() {
    uint i = gl_GlobalInvocationID.x;
    vec3 origin = q[i].xyz;

    rayQueryEXT rq;
    // Degenerate query: zero-length ray at the point. AABB candidates ARE the
    // spatial-index hits (BVH-as-index), not geometric ray intersections.
    rayQueryInitializeEXT(rq, scene, gl_RayFlagsNoneEXT, 0xFFu,
                          origin, 0.0, vec3(0.0, 0.0, 1.0), 0.0);

    uint count = 0u;
    while (rayQueryProceedEXT(rq)) {
        if (rayQueryGetIntersectionTypeEXT(rq, false)
                == gl_RayQueryCandidateIntersectionAABBEXT) {
            // Custom intersection / gather: visit this candidate data point.
            // A real index would fetch the point and test exact distance < radius.
            count += 1u;
            // Do not commit — keep traversing to visit every overlapping point.
        }
    }
    counts[i] = count;
}
)GLSL";

// Run a host tool, optionally capturing stdout to a file. Returns the exit code, or
// std::nullopt if the tool is not installed (caller SKIPs).
std::optional<int> runTool(const char* name,
                           const llvm::SmallVector<llvm::StringRef, 8>& args,
                           const std::string& stdoutPath = "") {
    auto tool = llvm::sys::findProgramByName(name);
    if (!tool) return std::nullopt;
    std::optional<llvm::StringRef> redirects[3] = {std::nullopt, std::nullopt,
                                                   std::nullopt};
    if (!stdoutPath.empty()) redirects[1] = llvm::StringRef(stdoutPath);
    return llvm::sys::ExecuteAndWait(*tool, args, /*Env=*/std::nullopt, redirects);
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

// The spatial-index ray-query pattern compiles to a Vulkan 1.3 SPIR-V module that is
// (a) spirv-val-clean and (b) a GLCompute entry point carrying the ray-query opcodes
// — proving the encoding needs no RT pipeline, only ray query in a compute shader.
TEST(GpuRayQueryProbe, spatialIndexComputeShaderIsValidSpirv) {
    if (!llvm::sys::findProgramByName("glslangValidator")) {
        GTEST_SKIP() << "glslangValidator not installed; ray-query expressibility "
                        "probe skipped";
    }

    static std::mt19937_64 rng(std::random_device{}());
    auto dir = std::filesystem::temp_directory_path()
             / ("cajeta_rq_" + std::to_string(rng()));
    std::filesystem::create_directories(dir);
    auto src = dir / "spatial_index.comp";
    auto spv = dir / "spatial_index.spv";
    auto dis = dir / "spatial_index.spvasm";
    { std::ofstream o(src); o << kRayQueryComputeGlsl; }

    // Compile GLSL compute → SPIR-V (vulkan1.3 implies SPIR-V 1.6, which ray query
    // requires). A failure here would mean the encoding isn't even expressible.
    std::string srcStr = src.string(), spvStr = spv.string();
    auto compileRc = runTool(
        "glslangValidator",
        {"glslangValidator", "--target-env", "vulkan1.3", "-S", "comp",
         srcStr, "-o", spvStr});
    ASSERT_TRUE(compileRc.has_value());
    ASSERT_EQ(*compileRc, 0) << "glslangValidator rejected the ray-query compute "
                                "shader — encoding not expressible";
    ASSERT_TRUE(std::filesystem::exists(spv));

    // Prove the module is a structurally VALID Vulkan 1.3 SPIR-V binary.
    if (auto valRc = runTool("spirv-val",
                             {"spirv-val", "--target-env", "vulkan1.3", spvStr})) {
        EXPECT_EQ(*valRc, 0) << "spirv-val rejected the ray-query compute module";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; binary validation skipped";
    }

    // Disassemble and assert the ray-query seam: ray query lives in a COMPUTE entry
    // point (no raygen/SBT), over an acceleration structure, inspecting AABB
    // candidates. This is the exact opcode set cajeta-gpu C3.3 must emit from LLVM.
    if (!llvm::sys::findProgramByName("spirv-dis")) {
        GTEST_SUCCEED() << "spirv-dis not installed; opcode assertions skipped";
        return;
    }
    auto disRc = runTool("spirv-dis", {"spirv-dis", spvStr}, dis.string());
    ASSERT_TRUE(disRc.has_value() && *disRc == 0);
    std::string asmText = slurp(dis);

    // Compute entry point — the headline: ray query WITHOUT the RT pipeline.
    EXPECT_NE(asmText.find("OpEntryPoint GLCompute"), std::string::npos) << asmText;
    // Extension + capability the C3.3 lowering must emit.
    EXPECT_NE(asmText.find("SPV_KHR_ray_query"), std::string::npos) << asmText;
    EXPECT_NE(asmText.find("OpCapability RayQueryKHR"), std::string::npos) << asmText;
    // The opaque types (added via the TargetExtType path, mirroring Image/Sampler).
    EXPECT_NE(asmText.find("OpTypeAccelerationStructureKHR"), std::string::npos)
        << asmText;
    EXPECT_NE(asmText.find("OpTypeRayQueryKHR"), std::string::npos) << asmText;
    // The query opcodes themselves: initialize, traverse, inspect candidate type.
    EXPECT_NE(asmText.find("OpRayQueryInitializeKHR"), std::string::npos) << asmText;
    EXPECT_NE(asmText.find("OpRayQueryProceedKHR"), std::string::npos) << asmText;
    EXPECT_NE(asmText.find("OpRayQueryGetIntersectionTypeKHR"), std::string::npos)
        << asmText;
}
