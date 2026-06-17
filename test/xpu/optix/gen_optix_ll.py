# Generates optix_progs_ll.ll — a hand-authored LLVM IR reproduction of the M0
# OptiX count-shape programs (optix_progs.cu), to prove the fork's NVPTX backend
# emits PTX that optixModuleCreate/optixPipelineCreate accept on the 4090.
# M2 Phase-1 feasibility spike (rayquery-optix-m2-codegen-plan.md 1a/1b).
import struct

# tmax = 0.001f, expressed as the double bit-pattern LLVM requires for a float literal.
f = struct.unpack('<f', struct.pack('<f', 0.001))[0]
TMAX = '0x%016X' % struct.unpack('<Q', struct.pack('<d', f))[0]

# ---- optixTrace inline asm (49-in / 32-out _optix_trace_typed_32) ----------
outs = ",".join("$%d" % i for i in range(32))          # $0..$31
ins  = ",".join("$%d" % i for i in range(32, 81))      # $32..$80
TRACE_TMPL = "call (%s), _optix_trace_typed_32, (%s);" % (outs, ins)
TRACE_RET  = "{ " + ", ".join(["i32"] * 32) + " }"
TRACE_CONS = ",".join(["=r"] * 32                      # 32 outputs
                      + ["r", "l"]                     # type, handle
                      + ["f"] * 9                      # ox,oy,oz,dx,dy,dz,tmin,tmax,time
                      + ["r"] * 6                      # mask,flags,sbtoff,sbtstride,misssbt,payloadsize
                      + ["r"] * 32)                    # p[1..32]
# trace call args: 17 fixed + 32 payload
trace_args = []
trace_args.append("i32 0")                # type (OptixPayloadTypeID 0)
trace_args.append("i64 %handle")          # traversable
trace_args += ["float %ox", "float %oy", "float %oz"]
trace_args += ["float 0.0", "float 0.0", "float 1.0"]      # dir (0,0,1)
trace_args += ["float 0.0", "float %s" % TMAX, "float 0.0"]  # tmin, tmax, time
trace_args += ["i32 255", "i32 0", "i32 0", "i32 1", "i32 0", "i32 1"]  # mask,flags,sbtoff,sbtstride,misssbt,payloadSize
trace_args += ["i32 0"]                    # p[1] in (initial count)
trace_args += ["i32 0"] * 31               # p[2..32] in
TRACE_ARGS = ", ".join(trace_args)

ll = f'''; Hand-authored OptiX count-shape programs (M2 Phase-1 spike).
; Mirrors test/xpu/optix/optix_progs.cu semantics so the existing M0 host harness
; (same params layout + entry names) can launch it and check the {{1,0,1,1}} oracle.
target datalayout = "e-i64:64-i128:128-v16:16-v32:32-n16:32:64"
target triple = "nvptx64-nvidia-cuda"

; extern "C" __constant__ Params params;  (40 bytes: handle, queries, counts, n, boxes)
@params = addrspace(4) global [40 x i8] zeroinitializer, align 8

define ptx_kernel void @__raygen__count() {{
entry:
  %i = call i32 asm sideeffect "call ($0), _optix_get_launch_index_x, ();", "=r"()
  %pn = getelementptr inbounds [40 x i8], ptr addrspace(4) @params, i64 0, i64 24
  %n = load i32, ptr addrspace(4) %pn, align 4
  %oob = icmp uge i32 %i, %n
  br i1 %oob, label %ret, label %body

body:
  %i64 = zext i32 %i to i64
  ; queries base (params+8), float3 stride 12
  %pq = getelementptr inbounds [40 x i8], ptr addrspace(4) @params, i64 0, i64 8
  %qraw = load i64, ptr addrspace(4) %pq, align 8
  %qbase = inttoptr i64 %qraw to ptr addrspace(1)
  %qoff = mul i64 %i64, 12
  %qe = getelementptr inbounds i8, ptr addrspace(1) %qbase, i64 %qoff
  %ox = load float, ptr addrspace(1) %qe, align 4
  %qe1 = getelementptr inbounds i8, ptr addrspace(1) %qe, i64 4
  %oy = load float, ptr addrspace(1) %qe1, align 4
  %qe2 = getelementptr inbounds i8, ptr addrspace(1) %qe, i64 8
  %oz = load float, ptr addrspace(1) %qe2, align 4
  ; handle (params+0)
  %handle = load i64, ptr addrspace(4) @params, align 8
  ; optixTrace
  %tr = call {TRACE_RET} asm sideeffect "{TRACE_TMPL}", "{TRACE_CONS}"({TRACE_ARGS})
  %count = extractvalue {TRACE_RET} %tr, 0
  ; counts[i] = count  (params+16)
  %pc = getelementptr inbounds [40 x i8], ptr addrspace(4) @params, i64 0, i64 16
  %craw = load i64, ptr addrspace(4) %pc, align 8
  %cbase = inttoptr i64 %craw to ptr addrspace(1)
  %coff = mul i64 %i64, 4
  %ce = getelementptr inbounds i8, ptr addrspace(1) %cbase, i64 %coff
  store i32 %count, ptr addrspace(1) %ce, align 4
  br label %ret

ret:
  ret void
}}

define ptx_kernel void @__intersection__box() {{
entry:
  %idx = call i32 asm sideeffect "call ($0), _optix_read_primitive_idx, ();", "=r"()
  %ox = call float asm sideeffect "call ($0), _optix_get_object_ray_origin_x, ();", "=f"()
  %oy = call float asm sideeffect "call ($0), _optix_get_object_ray_origin_y, ();", "=f"()
  %oz = call float asm sideeffect "call ($0), _optix_get_object_ray_origin_z, ();", "=f"()
  ; boxes base (params+32), 6 floats per prim -> stride 24
  %pb = getelementptr inbounds [40 x i8], ptr addrspace(4) @params, i64 0, i64 32
  %braw = load i64, ptr addrspace(4) %pb, align 8
  %bbase = inttoptr i64 %braw to ptr addrspace(1)
  %idx64 = zext i32 %idx to i64
  %boff = mul i64 %idx64, 24
  %be = getelementptr inbounds i8, ptr addrspace(1) %bbase, i64 %boff
  %b0 = load float, ptr addrspace(1) %be, align 4
  %be1 = getelementptr inbounds i8, ptr addrspace(1) %be, i64 4
  %b1 = load float, ptr addrspace(1) %be1, align 4
  %be2 = getelementptr inbounds i8, ptr addrspace(1) %be, i64 8
  %b2 = load float, ptr addrspace(1) %be2, align 4
  %be3 = getelementptr inbounds i8, ptr addrspace(1) %be, i64 12
  %b3 = load float, ptr addrspace(1) %be3, align 4
  %be4 = getelementptr inbounds i8, ptr addrspace(1) %be, i64 16
  %b4 = load float, ptr addrspace(1) %be4, align 4
  %be5 = getelementptr inbounds i8, ptr addrspace(1) %be, i64 20
  %b5 = load float, ptr addrspace(1) %be5, align 4
  %c0 = fcmp oge float %ox, %b0
  %c1 = fcmp oge float %oy, %b1
  %c2 = fcmp oge float %oz, %b2
  %c3 = fcmp ole float %ox, %b3
  %c4 = fcmp ole float %oy, %b4
  %c5 = fcmp ole float %oz, %b5
  %a0 = and i1 %c0, %c1
  %a1 = and i1 %a0, %c2
  %a2 = and i1 %a1, %c3
  %a3 = and i1 %a2, %c4
  %inside = and i1 %a3, %c5
  br i1 %inside, label %report, label %ret

report:
  %r = call i32 asm sideeffect "call ($0), _optix_report_intersection_0, ($1, $2);", "=r,f,r"(float 0.0, i32 0)
  br label %ret

ret:
  ret void
}}

define ptx_kernel void @__anyhit__count() {{
entry:
  %p = call i32 asm sideeffect "call ($0), _optix_get_payload, ($1);", "=r,r"(i32 0)
  %p1 = add i32 %p, 1
  call void asm sideeffect "call _optix_set_payload, ($0, $1);", "r,r"(i32 0, i32 %p1)
  call void asm sideeffect "call _optix_ignore_intersection, ();", ""()
  ret void
}}

define ptx_kernel void @__miss__nop() {{
  ret void
}}
'''

with open("optix_progs_ll.ll", "w") as fh:
    fh.write(ll)
print("wrote optix_progs_ll.ll  (tmax=%s)" % TMAX)
