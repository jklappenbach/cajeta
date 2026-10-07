#pragma once

namespace llvm {
class Function;
class Instruction;
class Value;
}

namespace cajeta {
namespace xpu {

// Tags `v`, an instruction or an argument, as a value that differs between the lanes of a workgroup.
void markVarying(llvm::Value* v);

// Tags `barrier` as the barrier of a Workgroup.reduce call site.
void markWorkgroupReduce(llvm::Instruction* barrier);

// True when some lane of a workgroup may not reach one of `kernel`'s Workgroup.reduce barriers,
// helpers included: one sits under a branch, loop exit or return whose condition varies by lane.
bool workgroupReduceUnderDivergence(llvm::Function& kernel);

} // namespace xpu
} // namespace cajeta
