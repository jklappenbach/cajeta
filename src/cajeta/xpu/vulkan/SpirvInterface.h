// SPIR-V shader interface variables (cajeta-gfx §4.b): the module-global Input/Output
// variables a graphics stage talks to the pipeline through, decorated with a Location or
// a BuiltIn. The graphics analog of the compute path's descriptor-bound storage buffers.
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

    // The address spaces the in-tree SPIR-V backend maps to StorageClass Input / Output.
    enum class InterfaceStorage : unsigned {
        Input  = 7,
        Output = 8,
    };

    // The SPIR-V Decoration opcodes used on interface variables.
    enum class SpirvDecoration : uint32_t {
        BuiltIn  = 11,
        Location = 30,
    };

    // The Khronos SPIR-V BuiltIn enumerants used by the vertex/fragment stages.
    enum class SpirvBuiltIn : uint32_t {
        Position      = 0,    // gl_Position   (vertex output)
        PointSize     = 1,    // gl_PointSize  (vertex output)
        FragCoord     = 15,   // gl_FragCoord  (fragment input)
        FrontFacing   = 17,   // gl_FrontFacing(fragment input)
        FragDepth     = 22,   // gl_FragDepth  (fragment output)
        VertexIndex   = 42,   // gl_VertexIndex   (vertex input)
        InstanceIndex = 43,   // gl_InstanceIndex (vertex input)
    };

    // Create an interface variable: a GlobalVariable of type `ty` in storage class `sc`
    // carrying one `!spirv.Decorations` pair. The generic builder the wrappers delegate to.
    llvm::GlobalVariable* createInterfaceVar(llvm::Module& m, llvm::Type* ty,
                                             InterfaceStorage sc,
                                             SpirvDecoration decoration,
                                             uint32_t operand,
                                             const std::string& name);

    // A Location-decorated interface variable at slot `location`.
    llvm::GlobalVariable* createLocationVar(llvm::Module& m, llvm::Type* ty,
                                            InterfaceStorage sc,
                                            uint32_t location,
                                            const std::string& name);

    // A BuiltIn-decorated interface variable (gl_Position, gl_FragCoord, …).
    llvm::GlobalVariable* createBuiltInVar(llvm::Module& m, llvm::Type* ty,
                                           InterfaceStorage sc,
                                           SpirvBuiltIn builtIn,
                                           const std::string& name);

    // The address space the in-tree SPIR-V backend maps to StorageClass PushConstant - a
    // third interface flavor, routed BY ADDRESS SPACE rather than by a decoration.
    constexpr unsigned kPushConstantAS = 13;

    // Create a push-constant block: a GlobalVariable of struct type `blockTy` in the
    // PushConstant address space. Vulkan permits exactly ONE per stage, so a shader's
    // @PushConstant params share this struct; the backend adds Block/Offset decorations.
    llvm::GlobalVariable* createPushConstantBlock(llvm::Module& m,
                                                  llvm::Type* blockTy,
                                                  const std::string& name);

} // namespace vulkan
} // namespace xpu
} // namespace cajeta
