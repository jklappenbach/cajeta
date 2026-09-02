#!/usr/bin/env swift
//
// metal-atomic64-probe.swift — does Metal actually support 64-bit atomics?
//
// WHY THIS EXISTS
// Apple's own documentation contradicts itself:
//
//   * Metal Shading Language Spec v4.1 (2026-06-04) §6.16.4.6 "Atomic Modify
//     Functions (64 Bits)" documents EXACTLY TWO ops — atomic_min_explicit and
//     atomic_max_explicit — on atomic_ulong, device address space only,
//     memory_order_relaxed only, RETURNING VOID.
//
//   * Metal Feature Set Tables (2026-05-21), footnote 7, says:
//     "The full set of 64-bit atomic operations is supported on all platforms
//      starting with Apple9."
//
// Those cannot both be true. The answer decides whether
// VK_KHR_shader_atomic_int64 is implementable on Metal at all — which is the
// single remaining gap in KosmicKrisp's Vulkan 1.2 row, and a four-year-old
// blocker on MoltenVK (issue #1692).
//
// Vulkan requires the RETURNING forms: OpAtomicUMin/OpAtomicUMax yield the
// previous value. A void-returning min/max cannot implement them, and bolting
// on a separate load defeats atomicity. So "does atomic_fetch_min_explicit
// exist for ulong" is the question, not "does atomic_min_explicit exist".
//
// USAGE
//   swift metal-atomic64-probe.swift
//
// Requires macOS with a Metal device. Apple9 = A17 Pro / A18 / M3 / M4.
// Run on an M3 or M4 for the load-bearing result; older parts are still useful
// as a control.
//
// Each operation is compiled in ISOLATION so one failure cannot mask another,
// then — if it compiled — actually executed and checked for correctness.
//

import Metal
import Foundation

// ---------------------------------------------------------------------------

guard let device = MTLCreateSystemDefaultDevice() else {
    print("FATAL: no Metal device (running in a VM without GPU passthrough?)")
    exit(1)
}
guard let queue = device.makeCommandQueue() else {
    print("FATAL: could not create command queue"); exit(1)
}

func familyName() -> String {
    let families: [(String, MTLGPUFamily)] = [
        ("Apple10", .apple10), ("Apple9", .apple9), ("Apple8", .apple8),
        ("Apple7", .apple7), ("Apple6", .apple6), ("Apple5", .apple5),
    ]
    let supported = families.filter { device.supportsFamily($0.1) }.map { $0.0 }
    return supported.isEmpty ? "none of Apple5-10" : supported.joined(separator: " ")
}

print("=== Metal 64-bit atomic probe ===")
print("device        : \(device.name)")
print("families      : \(familyName())")
print("unified mem   : \(device.hasUnifiedMemory)")
if #available(macOS 13.0, *) {
    print("Metal3        : \(device.supportsFamily(.metal3))")
}
print("OS            : \(ProcessInfo.processInfo.operatingSystemVersionString)")
print("")

// ---------------------------------------------------------------------------
// Each probe is a complete kernel. `expect` is what a CORRECT implementation
// must leave in out[0]; nil means "compile-only, correctness not checked".
// `prev` is the previous-value slot for fetch-style ops (out[1]).

struct Probe {
    let name: String
    let note: String
    let source: String
    let expect: UInt64?
    let expectPrev: UInt64?
}

// Common preamble. Buffer 0 is the atomic target, buffer 1 the witness for
// returned "previous" values.
func kernel(_ body: String, atomicType: String = "atomic_ulong",
            space: String = "device") -> String {
    """
    #include <metal_stdlib>
    using namespace metal;
    kernel void probe(device \(atomicType)* a       [[buffer(0)]],
                      device ulong*        prev     [[buffer(1)]],
                      uint tid                      [[thread_position_in_grid]])
    {
        \(body)
    }
    """
}

let probes: [Probe] = [

    // -- The two ops the MSL spec says exist. Void-returning. ----------------
    Probe(name: "atomic_min_explicit (void)",
          note: "MSL 4.1 §6.16.4.6 says this EXISTS. Control case.",
          source: kernel("""
              atomic_min_explicit(a, 40ul, memory_order_relaxed);
          """),
          expect: 40, expectPrev: nil),

    Probe(name: "atomic_max_explicit (void)",
          note: "MSL 4.1 §6.16.4.6 says this EXISTS. Control case.",
          source: kernel("""
              atomic_max_explicit(a, 999ul, memory_order_relaxed);
          """),
          expect: 999, expectPrev: nil),

    // -- The ops Vulkan actually needs. Returning forms. ---------------------
    Probe(name: "atomic_fetch_min_explicit",
          note: "REQUIRED for OpAtomicUMin. Spec says absent for ulong.",
          source: kernel("""
              prev[0] = atomic_fetch_min_explicit(a, 40ul, memory_order_relaxed);
          """),
          expect: 40, expectPrev: 100),

    Probe(name: "atomic_fetch_max_explicit",
          note: "REQUIRED for OpAtomicUMax. Spec says absent for ulong.",
          source: kernel("""
              prev[0] = atomic_fetch_max_explicit(a, 999ul, memory_order_relaxed);
          """),
          expect: 999, expectPrev: 100),

    Probe(name: "atomic_fetch_add_explicit",
          note: "THE headline test. OpAtomicIAdd. Spec says absent for ulong.",
          source: kernel("""
              prev[0] = atomic_fetch_add_explicit(a, 5ul, memory_order_relaxed);
          """),
          expect: 105, expectPrev: 100),

    Probe(name: "atomic_fetch_sub_explicit",
          note: "OpAtomicISub.",
          source: kernel("""
              prev[0] = atomic_fetch_sub_explicit(a, 5ul, memory_order_relaxed);
          """),
          expect: 95, expectPrev: 100),

    Probe(name: "atomic_fetch_and_explicit",
          note: "OpAtomicAnd.",
          source: kernel("""
              prev[0] = atomic_fetch_and_explicit(a, 0xFFul, memory_order_relaxed);
          """),
          expect: 100 & 0xFF, expectPrev: 100),

    Probe(name: "atomic_fetch_or_explicit",
          note: "OpAtomicOr.",
          source: kernel("""
              prev[0] = atomic_fetch_or_explicit(a, 0xF00ul, memory_order_relaxed);
          """),
          expect: 100 | 0xF00, expectPrev: 100),

    Probe(name: "atomic_fetch_xor_explicit",
          note: "OpAtomicXor.",
          source: kernel("""
              prev[0] = atomic_fetch_xor_explicit(a, 0xFFul, memory_order_relaxed);
          """),
          expect: 100 ^ 0xFF, expectPrev: 100),

    Probe(name: "atomic_exchange_explicit",
          note: "OpAtomicExchange.",
          source: kernel("""
              prev[0] = atomic_exchange_explicit(a, 777ul, memory_order_relaxed);
          """),
          expect: 777, expectPrev: 100),

    Probe(name: "atomic_compare_exchange_weak_explicit",
          note: "OpAtomicCompareExchange. The hardest to emulate if absent.",
          source: kernel("""
              ulong expected = 100ul;
              atomic_compare_exchange_weak_explicit(a, &expected, 555ul,
                  memory_order_relaxed, memory_order_relaxed);
              prev[0] = expected;
          """),
          expect: 555, expectPrev: 100),

    Probe(name: "atomic_load_explicit",
          note: "MSL §6.16.4.2 lists no ulong overload.",
          source: kernel("""
              prev[0] = atomic_load_explicit(a, memory_order_relaxed);
          """),
          expect: 100, expectPrev: 100),

    Probe(name: "atomic_store_explicit",
          note: "MSL §6.16.4.1 lists no ulong overload.",
          source: kernel("""
              atomic_store_explicit(a, 321ul, memory_order_relaxed);
          """),
          expect: 321, expectPrev: nil),

    // -- Scope: Vulkan's shaderSharedInt64Atomics needs threadgroup ----------
    Probe(name: "threadgroup atomic_fetch_add_explicit",
          note: "shaderSharedInt64Atomics. MSL restricts 64-bit to `device`.",
          source: """
              #include <metal_stdlib>
              using namespace metal;
              kernel void probe(device atomic_ulong* a   [[buffer(0)]],
                                device ulong*        prev [[buffer(1)]],
                                uint tid [[thread_position_in_grid]])
              {
                  threadgroup atomic_ulong shared_counter;
                  atomic_store_explicit(&shared_counter, 100ul, memory_order_relaxed);
                  threadgroup_barrier(mem_flags::mem_threadgroup);
                  prev[0] = atomic_fetch_add_explicit(&shared_counter, 5ul,
                                                      memory_order_relaxed);
                  threadgroup_barrier(mem_flags::mem_threadgroup);
                  atomic_store_explicit(a,
                      atomic_load_explicit(&shared_counter, memory_order_relaxed),
                      memory_order_relaxed);
              }
              """,
          expect: 105, expectPrev: 100),

    // -- Signed. MSL has no atomic<long> at all; confirm. --------------------
    Probe(name: "atomic_fetch_add_explicit (signed atomic_long)",
          note: "MSL §2.6 lists ulong but NOT long. OpAtomicSMin/SMax need it.",
          source: kernel("""
              prev[0] = (ulong)atomic_fetch_add_explicit(a, 5l, memory_order_relaxed);
          """, atomicType: "atomic_long"),
          expect: 105, expectPrev: 100),
]

// ---------------------------------------------------------------------------

enum Outcome {
    case compileFailed(String)
    case ranCorrect
    case ranWrong(got: UInt64, want: UInt64, gotPrev: UInt64?, wantPrev: UInt64?)
    case ranUnchecked(got: UInt64)
    case runtimeFailed(String)
}

func run(_ p: Probe) -> Outcome {
    // 1. Compile in isolation.
    let lib: MTLLibrary
    do {
        let opts = MTLCompileOptions()
        lib = try device.makeLibrary(source: p.source, options: opts)
    } catch {
        // First line only — Metal errors are long and the useful part is first.
        let msg = "\(error)".split(separator: "\n")
            .first(where: { $0.contains("error:") }) ?? "\(error)".prefix(160)
        return .compileFailed(String(msg).trimmingCharacters(in: .whitespaces))
    }
    guard let fn = lib.makeFunction(name: "probe"),
          let pipe = try? device.makeComputePipelineState(function: fn) else {
        return .runtimeFailed("pipeline creation failed")
    }

    // 2. Execute with a known starting value of 100.
    guard let aBuf = device.makeBuffer(length: 8, options: .storageModeShared),
          let pBuf = device.makeBuffer(length: 8, options: .storageModeShared) else {
        return .runtimeFailed("buffer allocation failed")
    }
    aBuf.contents().bindMemory(to: UInt64.self, capacity: 1)[0] = 100
    pBuf.contents().bindMemory(to: UInt64.self, capacity: 1)[0] = 0xDEAD

    guard let cb = queue.makeCommandBuffer(),
          let enc = cb.makeComputeCommandEncoder() else {
        return .runtimeFailed("encoder creation failed")
    }
    enc.setComputePipelineState(pipe)
    enc.setBuffer(aBuf, offset: 0, index: 0)
    enc.setBuffer(pBuf, offset: 0, index: 1)
    enc.dispatchThreads(MTLSize(width: 1, height: 1, depth: 1),
                        threadsPerThreadgroup: MTLSize(width: 1, height: 1, depth: 1))
    enc.endEncoding()
    cb.commit()
    cb.waitUntilCompleted()

    if let e = cb.error { return .runtimeFailed("\(e)") }

    let got = aBuf.contents().bindMemory(to: UInt64.self, capacity: 1)[0]
    let gotPrev = pBuf.contents().bindMemory(to: UInt64.self, capacity: 1)[0]

    guard let want = p.expect else { return .ranUnchecked(got: got) }
    let prevOK = p.expectPrev == nil || gotPrev == p.expectPrev!
    if got == want && prevOK { return .ranCorrect }
    return .ranWrong(got: got, want: want,
                     gotPrev: p.expectPrev == nil ? nil : gotPrev,
                     wantPrev: p.expectPrev)
}

// ---------------------------------------------------------------------------

var compiled = 0, correct = 0
print(String(repeating: "-", count: 78))
for p in probes {
    let r = run(p)
    let status: String
    switch r {
    case .compileFailed(let m):
        status = "COMPILE FAIL  \(m)"
    case .ranCorrect:
        compiled += 1; correct += 1
        status = "OK            compiled and produced the correct result"
    case .ranWrong(let g, let w, let gp, let wp):
        compiled += 1
        var d = "WRONG RESULT  value=\(g) expected=\(w)"
        if let gp = gp, let wp = wp { d += "  prev=\(gp) expected=\(wp)" }
        status = d
    case .ranUnchecked(let g):
        compiled += 1; correct += 1
        status = "OK            compiled, value=\(g)"
    case .runtimeFailed(let m):
        compiled += 1
        status = "RUNTIME FAIL  \(m)"
    }
    print("\(p.name)")
    print("   \(p.note)")
    print("   → \(status)")
    print("")
}
print(String(repeating: "-", count: 78))
print("compiled: \(compiled)/\(probes.count)    fully correct: \(correct)/\(probes.count)")
print("")
print("INTERPRETATION")
print("  If atomic_fetch_add/min/max_explicit COMPILE AND ARE CORRECT on ulong,")
print("  then the Feature Set Tables are right, the MSL spec is understating the")
print("  language, and VK_KHR_shader_atomic_int64 is implementable — which closes")
print("  KosmicKrisp's Vulkan 1.2 gap and retires MoltenVK issue #1692.")
print("")
print("  If only the void-returning atomic_min/max_explicit compile, the MSL spec")
print("  is accurate, and the extension is NOT implementable by any driver until")
print("  Apple ships the returning forms. Report that to Mesa #14251 too — a")
print("  definitive negative is worth publishing.")
