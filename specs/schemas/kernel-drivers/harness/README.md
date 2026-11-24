# kernel-drivers gate harness

Evidence for agents/kernel-drivers-plan.md 3.1, 3.3.1, 4.1 and 4.2.1. `ref/RefX8`, `ref/RefPack` and
`ref/QkTile` are cajeta-llm's kernels lifted unchanged (llm 9887052); `Bench` runs the cpu row driver
against `q4kQ8X8RowsKernel`, `TileBench` the GPU tile driver against `QkTile`. Each prints, per shape,
the count of differing output words and the min time of both kernels; the last line is `ALL EQUAL`
or `DIFFERENT`.

    KERNELS=~/cajeta-ml/kernels UNIT_CJA=.../dev.cajeta.unit-0.3.6.cja CAJETA=.../cajeta ./build.sh nvptx TileBench
    CAJETA_XPU_BACKEND=nvptx out-nvptx/TileBench

`Bench5` and `TileBench5` are the same two harnesses for Q5_K (plan 7.2 and 7.3), against
`ref/RefX8Q5` (`q5kQ8X8RowsKernel`) and `ref/Q5Tile`. They need cajeta with `Unroll.from`.

`Bench6` and `TileBench6` are the same for Q6_K (plan 8.1), against `ref/RefX8Q6`
(`q6kQ8X8RowsKernel`, timed with its 16-value group sums) and `ref/Q6Tile`, which reads cajeta-llm's
split row layout; the driver reads the file's blocks.

`Bench3`/`TileBench3` (Q3_K) and `Bench2`/`TileBench2` (Q2_K) follow the same pattern (plan 8.2).
`Q3Tile` reads the split row layout, `Q2Tile` the file's blocks. `Bench2` reports the pack size
difference without failing on it: the driver keeps scales and mins as two tables.

`VecBench` is the decode mat-vec (plan 9.1): the cpu row driver's one-token kernel against
cajeta-llm's `q{4,5,6,3,2}kQ8X8VecKernel` (`ref/RefVec*`), every K format, on the 8B and 4B shapes.

`Vec3Bench` is the fused three-output decode (plan 9.1.3) against `ref/RefVec3x` (`q4kQ8X8Vec3Kernel`).

`WaveBench` is the GPU decode mat-vec (plan 9.1.4): every member of `BlockGemvWave` against cajeta-llm's GPU decode kernels (`ref/RefWave`), best against best per shape; Q4_K and Q5_K pairs bitwise, the others by difference over the output scale.

`AttnBench` is the cpu decode attention (plan 9.3) against `ref/RefAttn` (`attnDecodeCpuGqaKernel`).
