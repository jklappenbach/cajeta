; cajeta_fp128_builtins.ll — IEEE-754 binary128 (fp128) soft-float conversion
; helpers, vendored as TARGET-NEUTRAL LLVM IR and linked into the embedded
; runtime bitcode (see src/CMakeLists.txt). The stdlib's Float128 type emits
; these compiler-rt calls (asFloat64 -> __trunctfdf2, asInt64 -> __fixtfdi,
; `(float128) <double>` -> __extenddftf2, ...). On x86_64-linux they resolve
; from the host libgcc via the JIT process-symbol generator, but Apple arm64
; has NO __float128 / fp128 support (clang rejects the type, Apple compiler-rt
; omits the tf family), so the JIT could not materialize ANY fp128-touching
; module -> "Symbols not found: [__trunctfdf2, __fixtfdi]" failed all 424 macOS
; release tests. Providing the definitions IN the embedded bitcode resolves the
; calls internally on every target, with no libgcc/compiler-rt dependency.
;
; The bodies are PURE INTEGER bit-manipulation (bitcast fp128<->i128 + int ops,
; verified: zero fptrunc/fpext/fptosi/fp-arith), so they are target-independent
; and JIT-codegen correctly on aarch64 even though the C front-end there can't
; spell the fp128 type. Strong external linkage (like the rest of the embedded
; runtime) so llvm-link retains them when merging into cajeta_runtime.bc even
; though nothing in that module references them yet — the references come from
; the per-module JIT'd Float128 code linked in later. No collision with a host
; libgcc copy: the JIT module's own definition resolves the call, so the
; process-symbol generator is never consulted for these names.
;
; REGENERATION (do not hand-edit): from the cajeta-llvm compiler-rt builtins,
;   for b in extenddftf2 extendsftf2 trunctfdf2 trunctfsf2 fixtfdi fixtfsi \
;            fixunstfdi fixunstfsi floatsitf floatditf floatunsitf floatunditf; do
;     clang-23 -c -emit-llvm -O2 -fno-builtin -ffreestanding \
;       --target=x86_64-linux-gnu -I<compiler-rt/lib/builtins> $b.c -o $b.bc
;   done
;   llvm-link *.bc -o all.bc && llvm-dis all.bc
;   # strip `target datalayout`/`target triple`, the x86 target-cpu/features
;   # attribute strings, and rewrite `define dso_local` -> `define linkonce_odr
;   # dso_local`.
;
; ModuleID = 'all.bc'
source_filename = "llvm-link"

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none)
define dso_local fp128 @__extenddftf2(double noundef %0) local_unnamed_addr #0 {
  %2 = bitcast double %0 to i64
  %3 = lshr i64 %2, 52
  %4 = and i64 %3, 2047
  %5 = and i64 %2, 4503599627370495
  switch i64 %4, label %6 [
    i64 2047, label %11
    i64 0, label %14
  ]

6:                                                ; preds = %1
  %7 = add nuw nsw i64 %4, 15360
  %8 = zext nneg i64 %7 to i128
  %9 = zext nneg i64 %5 to i128
  %10 = shl nuw nsw i128 %9, 60
  br label %26

11:                                               ; preds = %1
  %12 = zext nneg i64 %5 to i128
  %13 = shl nuw nsw i128 %12, 60
  br label %26

14:                                               ; preds = %1
  %15 = icmp eq i64 %5, 0
  br i1 %15, label %26, label %16

16:                                               ; preds = %14
  %17 = tail call range(i64 12, 65) i64 @llvm.ctlz.i64(i64 range(i64 1, 4503599627370496) %5, i1 true)
  %18 = trunc nuw nsw i64 %17 to i32
  %19 = sub nuw nsw i32 15372, %18
  %20 = zext nneg i32 %19 to i128
  %21 = zext nneg i64 %5 to i128
  %22 = add nuw nsw i32 %18, 49
  %23 = zext nneg i32 %22 to i128
  %24 = shl i128 %21, %23
  %25 = xor i128 %24, 5192296858534827628530496329220096
  br label %26

26:                                               ; preds = %6, %11, %14, %16
  %27 = phi i128 [ %10, %6 ], [ %13, %11 ], [ %25, %16 ], [ 0, %14 ]
  %28 = phi i128 [ %8, %6 ], [ 32767, %11 ], [ %20, %16 ], [ 0, %14 ]
  %29 = lshr i64 %2, 63
  %30 = zext nneg i64 %29 to i128
  %31 = shl nuw i128 %30, 127
  %32 = shl nuw nsw i128 %28, 112
  %33 = or disjoint i128 %32, %31
  %34 = or i128 %33, %27
  %35 = bitcast i128 %34 to fp128
  ret fp128 %35
}

; Function Attrs: nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare i64 @llvm.ctlz.i64(i64, i1 immarg) #1

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none)
define dso_local fp128 @__extendsftf2(float noundef %0) local_unnamed_addr #0 {
  %2 = bitcast float %0 to i32
  %3 = lshr i32 %2, 23
  %4 = and i32 %2, 8388607
  %5 = trunc i32 %3 to i8
  switch i8 %5, label %6 [
    i8 -1, label %11
    i8 0, label %14
  ]

6:                                                ; preds = %1
  %7 = and i32 %3, 255
  %8 = add nuw nsw i32 %7, 16256
  %9 = zext nneg i32 %4 to i128
  %10 = shl nuw nsw i128 %9, 89
  br label %24

11:                                               ; preds = %1
  %12 = zext nneg i32 %4 to i128
  %13 = shl nuw nsw i128 %12, 89
  br label %24

14:                                               ; preds = %1
  %15 = icmp eq i32 %4, 0
  br i1 %15, label %24, label %16

16:                                               ; preds = %14
  %17 = tail call range(i32 9, 33) i32 @llvm.ctlz.i32(i32 range(i32 1, 8388608) %4, i1 true)
  %18 = sub nuw nsw i32 16265, %17
  %19 = zext nneg i32 %4 to i128
  %20 = add nuw nsw i32 %17, 81
  %21 = zext nneg i32 %20 to i128
  %22 = shl i128 %19, %21
  %23 = xor i128 %22, 5192296858534827628530496329220096
  br label %24

24:                                               ; preds = %6, %11, %14, %16
  %25 = phi i128 [ %10, %6 ], [ %13, %11 ], [ %23, %16 ], [ 0, %14 ]
  %26 = phi i32 [ %8, %6 ], [ 32767, %11 ], [ %18, %16 ], [ 0, %14 ]
  %27 = lshr i32 %2, 31
  %28 = zext nneg i32 %26 to i128
  %29 = zext nneg i32 %27 to i128
  %30 = shl nuw i128 %29, 127
  %31 = shl nuw nsw i128 %28, 112
  %32 = or disjoint i128 %31, %30
  %33 = or i128 %32, %25
  %34 = bitcast i128 %33 to fp128
  ret fp128 %34
}

; Function Attrs: nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare i32 @llvm.ctlz.i32(i32, i1 immarg) #1

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none)
define dso_local i64 @__fixtfdi(fp128 noundef %0) local_unnamed_addr #0 {
  %2 = bitcast fp128 %0 to i128
  %3 = icmp sgt i128 %2, -1
  %4 = lshr i128 %2, 112
  %5 = trunc nuw nsw i128 %4 to i32
  %6 = and i32 %5, 32767
  %7 = and i128 %2, 5192296858534827628530496329220095
  %8 = or disjoint i128 %7, 5192296858534827628530496329220096
  %9 = icmp samesign ult i32 %6, 16383
  br i1 %9, label %22, label %10

10:                                               ; preds = %1
  %11 = add nsw i32 %6, -16447
  %12 = icmp ult i32 %11, -64
  br i1 %12, label %13, label %15

13:                                               ; preds = %10
  %14 = select i1 %3, i64 9223372036854775807, i64 -9223372036854775808
  br label %22

15:                                               ; preds = %10
  %16 = sub nuw nsw i32 16495, %6
  %17 = zext nneg i32 %16 to i128
  %18 = lshr i128 %8, %17
  %19 = trunc nuw i128 %18 to i64
  %20 = sub i64 0, %19
  %21 = select i1 %3, i64 %19, i64 %20
  br label %22

22:                                               ; preds = %1, %13, %15
  %23 = phi i64 [ %21, %15 ], [ %14, %13 ], [ 0, %1 ]
  ret i64 %23
}

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none)
define dso_local i32 @__fixtfsi(fp128 noundef %0) local_unnamed_addr #0 {
  %2 = bitcast fp128 %0 to i128
  %3 = icmp sgt i128 %2, -1
  %4 = lshr i128 %2, 112
  %5 = trunc nuw nsw i128 %4 to i32
  %6 = and i32 %5, 32767
  %7 = and i128 %2, 5192296858534827628530496329220095
  %8 = or disjoint i128 %7, 5192296858534827628530496329220096
  %9 = icmp samesign ult i32 %6, 16383
  br i1 %9, label %22, label %10

10:                                               ; preds = %1
  %11 = add nsw i32 %6, -16415
  %12 = icmp ult i32 %11, -32
  br i1 %12, label %13, label %15

13:                                               ; preds = %10
  %14 = select i1 %3, i32 2147483647, i32 -2147483648
  br label %22

15:                                               ; preds = %10
  %16 = sub nuw nsw i32 16495, %6
  %17 = zext nneg i32 %16 to i128
  %18 = lshr i128 %8, %17
  %19 = trunc nuw i128 %18 to i32
  %20 = sub i32 0, %19
  %21 = select i1 %3, i32 %19, i32 %20
  br label %22

22:                                               ; preds = %1, %13, %15
  %23 = phi i32 [ %21, %15 ], [ %14, %13 ], [ 0, %1 ]
  ret i32 %23
}

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none)
define dso_local i64 @__fixunstfdi(fp128 noundef %0) local_unnamed_addr #0 {
  %2 = bitcast fp128 %0 to i128
  %3 = icmp slt i128 %2, 0
  %4 = lshr i128 %2, 112
  %5 = trunc nuw nsw i128 %4 to i32
  %6 = and i32 %5, 32767
  %7 = and i128 %2, 5192296858534827628530496329220095
  %8 = or disjoint i128 %7, 5192296858534827628530496329220096
  %9 = icmp samesign ult i32 %6, 16383
  %10 = select i1 %3, i1 true, i1 %9
  br i1 %10, label %19, label %11

11:                                               ; preds = %1
  %12 = add nsw i32 %6, -16447
  %13 = icmp ult i32 %12, -64
  br i1 %13, label %19, label %14

14:                                               ; preds = %11
  %15 = sub nuw nsw i32 16495, %6
  %16 = zext nneg i32 %15 to i128
  %17 = lshr i128 %8, %16
  %18 = trunc nuw i128 %17 to i64
  br label %19

19:                                               ; preds = %1, %11, %14
  %20 = phi i64 [ %18, %14 ], [ 0, %1 ], [ -1, %11 ]
  ret i64 %20
}

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none)
define dso_local i32 @__fixunstfsi(fp128 noundef %0) local_unnamed_addr #0 {
  %2 = bitcast fp128 %0 to i128
  %3 = icmp slt i128 %2, 0
  %4 = lshr i128 %2, 112
  %5 = trunc nuw nsw i128 %4 to i32
  %6 = and i32 %5, 32767
  %7 = and i128 %2, 5192296858534827628530496329220095
  %8 = or disjoint i128 %7, 5192296858534827628530496329220096
  %9 = icmp samesign ult i32 %6, 16383
  %10 = select i1 %3, i1 true, i1 %9
  br i1 %10, label %19, label %11

11:                                               ; preds = %1
  %12 = add nsw i32 %6, -16415
  %13 = icmp ult i32 %12, -32
  br i1 %13, label %19, label %14

14:                                               ; preds = %11
  %15 = sub nuw nsw i32 16495, %6
  %16 = zext nneg i32 %15 to i128
  %17 = lshr i128 %8, %16
  %18 = trunc nuw i128 %17 to i32
  br label %19

19:                                               ; preds = %1, %11, %14
  %20 = phi i32 [ %18, %14 ], [ 0, %1 ], [ -1, %11 ]
  ret i32 %20
}

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none)
define dso_local fp128 @__floatditf(i64 noundef %0) local_unnamed_addr #0 {
  %2 = icmp eq i64 %0, 0
  br i1 %2, label %21, label %3

3:                                                ; preds = %1
  %4 = icmp slt i64 %0, 0
  %5 = tail call i64 @llvm.abs.i64(i64 %0, i1 false)
  %6 = select i1 %4, i128 -170141183460469231731687303715884105728, i128 0
  %7 = tail call range(i64 0, 65) i64 @llvm.ctlz.i64(i64 %5, i1 true)
  %8 = trunc nuw nsw i64 %7 to i32
  %9 = xor i32 %8, 63
  %10 = sub nuw nsw i32 112, %9
  %11 = zext i64 %5 to i128
  %12 = zext nneg i32 %10 to i128
  %13 = shl i128 %11, %12
  %14 = xor i128 %13, 5192296858534827628530496329220096
  %15 = sub nuw nsw i32 16446, %8
  %16 = zext nneg i32 %15 to i128
  %17 = shl nuw nsw i128 %16, 112
  %18 = add i128 %14, %17
  %19 = or i128 %18, %6
  %20 = bitcast i128 %19 to fp128
  br label %21

21:                                               ; preds = %1, %3
  %22 = phi fp128 [ %20, %3 ], [ 0.000000e+00, %1 ]
  ret fp128 %22
}

; Function Attrs: nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare i64 @llvm.abs.i64(i64, i1 immarg) #1

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none)
define dso_local fp128 @__floatsitf(i32 noundef %0) local_unnamed_addr #0 {
  %2 = icmp eq i32 %0, 0
  br i1 %2, label %20, label %3

3:                                                ; preds = %1
  %4 = icmp slt i32 %0, 0
  %5 = tail call i32 @llvm.abs.i32(i32 %0, i1 false)
  %6 = select i1 %4, i128 -170141183460469231731687303715884105728, i128 0
  %7 = tail call range(i32 0, 33) i32 @llvm.ctlz.i32(i32 %5, i1 true)
  %8 = xor i32 %7, 31
  %9 = sub nuw nsw i32 112, %8
  %10 = zext i32 %5 to i128
  %11 = zext nneg i32 %9 to i128
  %12 = shl i128 %10, %11
  %13 = xor i128 %12, 5192296858534827628530496329220096
  %14 = sub nuw nsw i32 16414, %7
  %15 = zext nneg i32 %14 to i128
  %16 = shl nuw nsw i128 %15, 112
  %17 = add i128 %13, %16
  %18 = or i128 %17, %6
  %19 = bitcast i128 %18 to fp128
  br label %20

20:                                               ; preds = %1, %3
  %21 = phi fp128 [ %19, %3 ], [ 0.000000e+00, %1 ]
  ret fp128 %21
}

; Function Attrs: nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare i32 @llvm.abs.i32(i32, i1 immarg) #1

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none)
define dso_local fp128 @__floatunditf(i64 noundef %0) local_unnamed_addr #0 {
  %2 = icmp eq i64 %0, 0
  br i1 %2, label %17, label %3

3:                                                ; preds = %1
  %4 = tail call range(i64 0, 65) i64 @llvm.ctlz.i64(i64 %0, i1 true)
  %5 = trunc nuw nsw i64 %4 to i32
  %6 = xor i32 %5, 63
  %7 = sub nuw nsw i32 112, %6
  %8 = zext i64 %0 to i128
  %9 = zext nneg i32 %7 to i128
  %10 = shl i128 %8, %9
  %11 = xor i128 %10, 5192296858534827628530496329220096
  %12 = sub nuw nsw i32 16446, %5
  %13 = zext nneg i32 %12 to i128
  %14 = shl nuw nsw i128 %13, 112
  %15 = add i128 %11, %14
  %16 = bitcast i128 %15 to fp128
  br label %17

17:                                               ; preds = %1, %3
  %18 = phi fp128 [ %16, %3 ], [ 0.000000e+00, %1 ]
  ret fp128 %18
}

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none)
define dso_local fp128 @__floatunsitf(i32 noundef %0) local_unnamed_addr #0 {
  %2 = icmp eq i32 %0, 0
  br i1 %2, label %16, label %3

3:                                                ; preds = %1
  %4 = tail call range(i32 0, 33) i32 @llvm.ctlz.i32(i32 %0, i1 true)
  %5 = xor i32 %4, 31
  %6 = sub nuw nsw i32 112, %5
  %7 = zext i32 %0 to i128
  %8 = zext nneg i32 %6 to i128
  %9 = shl i128 %7, %8
  %10 = xor i128 %9, 5192296858534827628530496329220096
  %11 = sub nuw nsw i32 16414, %4
  %12 = zext nneg i32 %11 to i128
  %13 = shl nuw nsw i128 %12, 112
  %14 = add i128 %10, %13
  %15 = bitcast i128 %14 to fp128
  br label %16

16:                                               ; preds = %1, %3
  %17 = phi fp128 [ %15, %3 ], [ 0.000000e+00, %1 ]
  ret fp128 %17
}

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none)
define dso_local double @__trunctfdf2(fp128 noundef %0) local_unnamed_addr #0 {
  %2 = bitcast fp128 %0 to i128
  %3 = lshr i128 %2, 112
  %4 = and i128 %3, 32767
  %5 = and i128 %2, 5192296858534827628530496329220095
  %6 = trunc nuw nsw i128 %4 to i32
  %7 = add nsw i32 %6, -15361
  %8 = icmp ult i32 %7, 2046
  br i1 %8, label %9, label %29

9:                                                ; preds = %1
  %10 = add nsw i32 %6, -15360
  %11 = zext nneg i32 %10 to i64
  %12 = lshr i128 %5, 60
  %13 = trunc nuw nsw i128 %12 to i64
  %14 = and i128 %2, 1152921504606846975
  %15 = icmp samesign ugt i128 %14, 576460752303423488
  br i1 %15, label %16, label %18

16:                                               ; preds = %9
  %17 = add nuw nsw i64 %13, 1
  br label %23

18:                                               ; preds = %9
  %19 = icmp eq i128 %14, 576460752303423488
  br i1 %19, label %20, label %23

20:                                               ; preds = %18
  %21 = and i64 %13, 1
  %22 = add nuw nsw i64 %21, %13
  br label %23

23:                                               ; preds = %20, %18, %16
  %24 = phi i64 [ %17, %16 ], [ %22, %20 ], [ %13, %18 ]
  %25 = icmp samesign ugt i64 %24, 4503599627370495
  %26 = select i1 %25, i64 0, i64 %24
  %27 = zext i1 %25 to i64
  %28 = add nuw nsw i64 %27, %11
  br label %74

29:                                               ; preds = %1
  %30 = icmp eq i128 %4, 32767
  %31 = icmp ne i128 %5, 0
  %32 = and i1 %31, %30
  br i1 %32, label %33, label %37

33:                                               ; preds = %29
  %34 = lshr i128 %5, 60
  %35 = trunc nuw nsw i128 %34 to i64
  %36 = or i64 %35, 2251799813685248
  br label %74

37:                                               ; preds = %29
  %38 = icmp samesign ugt i32 %6, 17406
  br i1 %38, label %74, label %39

39:                                               ; preds = %37
  %40 = icmp eq i128 %4, 0
  %41 = select i1 %40, i32 15360, i32 15361
  %42 = sub nsw i32 %41, %6
  %43 = icmp sgt i32 %42, 112
  br i1 %43, label %74, label %44

44:                                               ; preds = %39
  %45 = or disjoint i128 %5, 5192296858534827628530496329220096
  %46 = select i1 %40, i128 %5, i128 %45
  %47 = icmp ne i32 %41, %6
  %48 = sub nsw i32 128, %42
  %49 = zext nneg i32 %48 to i128
  %50 = shl i128 %46, %49
  %51 = icmp ne i128 %50, 0
  %52 = select i1 %47, i1 %51, i1 false
  %53 = zext nneg i32 %42 to i128
  %54 = lshr i128 %46, %53
  %55 = zext i1 %52 to i128
  %56 = lshr i128 %54, 60
  %57 = trunc nuw nsw i128 %56 to i64
  %58 = and i128 %54, 1152921504606846975
  %59 = or i128 %58, %55
  %60 = icmp samesign ugt i128 %59, 576460752303423488
  br i1 %60, label %61, label %63

61:                                               ; preds = %44
  %62 = add nuw nsw i64 %57, 1
  br label %68

63:                                               ; preds = %44
  %64 = icmp eq i128 %59, 576460752303423488
  br i1 %64, label %65, label %68

65:                                               ; preds = %63
  %66 = and i64 %57, 1
  %67 = add nuw nsw i64 %66, %57
  br label %68

68:                                               ; preds = %65, %63, %61
  %69 = phi i64 [ %62, %61 ], [ %67, %65 ], [ %57, %63 ]
  %70 = icmp samesign ugt i64 %69, 4503599627370495
  %71 = xor i64 %69, 4503599627370496
  %72 = select i1 %70, i64 %71, i64 %69
  %73 = zext i1 %70 to i64
  br label %74

74:                                               ; preds = %23, %33, %37, %39, %68
  %75 = phi i64 [ %26, %23 ], [ %36, %33 ], [ 0, %37 ], [ %72, %68 ], [ 0, %39 ]
  %76 = phi i64 [ %28, %23 ], [ 2047, %33 ], [ 2047, %37 ], [ %73, %68 ], [ 0, %39 ]
  %77 = lshr i128 %2, 64
  %78 = trunc nuw i128 %77 to i64
  %79 = and i64 %78, -9223372036854775808
  %80 = shl nuw nsw i64 %76, 52
  %81 = or disjoint i64 %80, %79
  %82 = or i64 %81, %75
  %83 = bitcast i64 %82 to double
  ret double %83
}

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none)
define dso_local float @__trunctfsf2(fp128 noundef %0) local_unnamed_addr #0 {
  %2 = bitcast fp128 %0 to i128
  %3 = lshr i128 %2, 112
  %4 = and i128 %3, 32767
  %5 = and i128 %2, 5192296858534827628530496329220095
  %6 = trunc nuw nsw i128 %4 to i32
  %7 = add nsw i32 %6, -16257
  %8 = icmp ult i32 %7, 254
  br i1 %8, label %9, label %27

9:                                                ; preds = %1
  %10 = lshr i128 %5, 89
  %11 = trunc nuw nsw i128 %10 to i32
  %12 = and i128 %2, 618970019642690137449562111
  %13 = icmp samesign ugt i128 %12, 309485009821345068724781056
  br i1 %13, label %14, label %16

14:                                               ; preds = %9
  %15 = add nuw nsw i32 %11, 1
  br label %21

16:                                               ; preds = %9
  %17 = icmp eq i128 %12, 309485009821345068724781056
  br i1 %17, label %18, label %21

18:                                               ; preds = %16
  %19 = and i32 %11, 1
  %20 = add nuw nsw i32 %19, %11
  br label %21

21:                                               ; preds = %18, %16, %14
  %22 = phi i32 [ %15, %14 ], [ %20, %18 ], [ %11, %16 ]
  %23 = icmp samesign ugt i32 %22, 8388607
  %24 = select i1 %23, i32 0, i32 %22
  %25 = select i1 %23, i32 -16255, i32 -16256
  %26 = add nsw i32 %25, %6
  br label %72

27:                                               ; preds = %1
  %28 = icmp eq i128 %4, 32767
  %29 = icmp ne i128 %5, 0
  %30 = and i1 %29, %28
  br i1 %30, label %31, label %35

31:                                               ; preds = %27
  %32 = lshr i128 %5, 89
  %33 = trunc nuw nsw i128 %32 to i32
  %34 = or i32 %33, 4194304
  br label %72

35:                                               ; preds = %27
  %36 = icmp samesign ugt i32 %6, 16510
  br i1 %36, label %72, label %37

37:                                               ; preds = %35
  %38 = icmp eq i128 %4, 0
  %39 = select i1 %38, i32 16256, i32 16257
  %40 = sub nsw i32 %39, %6
  %41 = icmp sgt i32 %40, 112
  br i1 %41, label %72, label %42

42:                                               ; preds = %37
  %43 = or disjoint i128 %5, 5192296858534827628530496329220096
  %44 = select i1 %38, i128 %5, i128 %43
  %45 = icmp ne i32 %39, %6
  %46 = sub nsw i32 128, %40
  %47 = zext nneg i32 %46 to i128
  %48 = shl i128 %44, %47
  %49 = icmp ne i128 %48, 0
  %50 = select i1 %45, i1 %49, i1 false
  %51 = zext nneg i32 %40 to i128
  %52 = lshr i128 %44, %51
  %53 = zext i1 %50 to i128
  %54 = lshr i128 %52, 89
  %55 = trunc nuw nsw i128 %54 to i32
  %56 = and i128 %52, 618970019642690137449562111
  %57 = or i128 %56, %53
  %58 = icmp samesign ugt i128 %57, 309485009821345068724781056
  br i1 %58, label %59, label %61

59:                                               ; preds = %42
  %60 = add nuw nsw i32 %55, 1
  br label %66

61:                                               ; preds = %42
  %62 = icmp eq i128 %57, 309485009821345068724781056
  br i1 %62, label %63, label %66

63:                                               ; preds = %61
  %64 = and i32 %55, 1
  %65 = add nuw nsw i32 %64, %55
  br label %66

66:                                               ; preds = %63, %61, %59
  %67 = phi i32 [ %60, %59 ], [ %65, %63 ], [ %55, %61 ]
  %68 = icmp samesign ugt i32 %67, 8388607
  %69 = xor i32 %67, 8388608
  %70 = select i1 %68, i32 %69, i32 %67
  %71 = zext i1 %68 to i32
  br label %72

72:                                               ; preds = %21, %31, %35, %37, %66
  %73 = phi i32 [ %24, %21 ], [ %34, %31 ], [ 0, %35 ], [ %70, %66 ], [ 0, %37 ]
  %74 = phi i32 [ %26, %21 ], [ 255, %31 ], [ 255, %35 ], [ %71, %66 ], [ 0, %37 ]
  %75 = lshr i128 %2, 96
  %76 = trunc nuw i128 %75 to i32
  %77 = and i32 %76, -2147483648
  %78 = shl nuw nsw i32 %74, 23
  %79 = or disjoint i32 %78, %77
  %80 = or i32 %79, %73
  %81 = bitcast i32 %80 to float
  ret float %81
}

attributes #0 = { mustprogress nofree norecurse nosync nounwind willreturn memory(none) "no-builtins" "no-trapping-math"="true" }
attributes #1 = { nocallback nofree nosync nounwind speculatable willreturn memory(none) }

!llvm.ident = !{!0}
!llvm.errno.tbaa = !{!1}
!llvm.module.flags = !{!5, !6}

!0 = !{!"clang version 23.0.0git (git@github.com:jklappenbach/cajeta-llvm.git 88a274548e83647ef7c99f31b36c6850a5d85a47)"}
!1 = !{!2, !2, i64 0}
!2 = !{!"int", !3, i64 0}
!3 = !{!"omnipotent char", !4, i64 0}
!4 = !{!"Simple C/C++ TBAA"}
!5 = !{i32 8, !"PIC Level", i32 2}
!6 = !{i32 7, !"PIE Level", i32 2}
