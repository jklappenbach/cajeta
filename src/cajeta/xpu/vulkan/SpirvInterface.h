//
// SPIR-V shader interface variables (cajeta-gfx §4.b).
//
// A graphics shader stage talks to the pipeline through INTERFACE VARIABLES —
// module-global variables in the Input / Output storage classes, decorated with
// either a Location (vertex attributes, fragment color targets, inter-stage
// varyings) or a BuiltIn (gl_Position, gl_FragCoord, gl_VertexIndex, …). This is
// the graphics analog of the compute path's descriptor-bound storage buffers
// (SpirvKernelLowering): where a @Kernel reads its args from descriptors, a
// @Vertex/@Fragment shader reads inputs from / writes outputs to these globals.
//
// The in-tree LLVM SPIR-V backend encodes an interface variable as a
// GlobalVariable in addrspace 7 (Input) / 8 (Output) carrying `!spirv.Decorations`
// metadata — a node of {i32 decoration, i32 operand} pairs — exactly the form
// LLVM's own position.{vs,ps}.ll uses and that GfxSpirvEmitProbeTests proved
// emits spirv-val-clean. These builders are that pattern as a typed API for the
// graphics lowerer to call.
//

#pragma once

#include <cstdint>
#include <string>

namespace llvm {
    class Module;
    class Type;
    class GlobalVariable;
}

namespace cajeta {
namespace xpu {
namespace vulkan {

    // The interface storage classes, as the address spaces the in-tree SPIR-V
    // backend maps to StorageClass Input / Output.
    enum class InterfaceStorage : unsigned {
        Input  = 7,
        Output = 8,
    };

    // The SPIR-V Decoration opcodes used on interface variables.
    enum class SpirvDecoration : uint32_t {
        BuiltIn  = 11,
        Location = 30,
    };

    // The SPIR-V BuiltIn values relevant to the vertex/fragment stages (the set
    // the §4.b lowering needs; extend as later stages land). Values are the
    // Khronos SPIR-V BuiltIn enumerants.
    enum class SpirvBuiltIn : uint32_t {
        Position      = 0,    // gl_Position   (vertex output)
        PointSize     = 1,    // gl_PointSize  (vertex output)
        FragCoord     = 15,   // gl_FragCoord  (fragment input)
        FrontFacing   = 17,   // gl_FrontFacing(fragment input)
        FragDepth     = 22,   // gl_FragDepth  (fragment output)
        VertexIndex   = 42,   // gl_VertexIndex   (vertex input)
        InstanceIndex = 43,   // gl_InstanceIndex (vertex input)
    };

    // Create an interface variable: a GlobalVariable of type `ty` in storage
    // class `sc` (Input → read-only `constant`, Output → writable `global`),
    // carrying a single `!spirv.Decorations` pair {decoration, operand}. The
    // generic builder the two typed wrappers below delegate to.
    llvm::GlobalVariable* createInterfaceVar(llvm::Module& m, llvm::Type* ty,
                                             InterfaceStorage sc,
                                             SpirvDecoration decoration,
                                             uint32_t operand,
                                             const std::string& name);

    // A Location-decorated interface variable (vertex attribute, fragment color
    // output, or an inter-stage varying at slot `location`).
    llvm::GlobalVariable* createLocationVar(llvm::Module& m, llvm::Type* ty,
                                            InterfaceStorage sc,
                                            uint32_t location,
                                            const std::string& name);

    // A BuiltIn-decorated interface variable (gl_Position, gl_FragCoord, …).
    llvm::GlobalVariable* createBuiltInVar(llvm::Module& m, llvm::Type* ty,
                                           InterfaceStorage sc,
                                           SpirvBuiltIn builtIn,
                                           const std::string& name);

} // namespace vulkan
} // namespace xpu
} // namespace cajeta
