# MeshSimplifier

`cajeta.xpu.mesh.MeshSimplifier` — the appearance-preserving Garland–Heckbert
edge-collapse mesh simplifier, built on `Qem`. A mesh is an indexed triangle
soup in the canonical vertex-buffer layout: a flat `float32[]` of vertex
positions (x, y, z interleaved) and an `int32[]` of triangle corner indices
(three per triangle). The simplifier proceeds in two phases: accumulate a
per-vertex error quadric (`accumulateQuadrics`), then repeatedly collapse the
cheapest edge (`simplify`), moving each merged vertex to its error-minimizing
position — so flat regions collapse at zero cost while curved and creased
regions resist. Quadrics live in a flat `float32[]` (16 row-major entries per
vertex) because a `Matrix[]` cannot be allocated; `vertexQuadric` reconstructs
one on demand.

```cajeta
// Two triangles sharing an edge: 4 vertices (x, y, z each), 6 indices.
float32[] positions = heap float32[12];
positions[3] = 1.0f;                      // v1 = (1, 0, 0)
positions[7] = 1.0f;                      // v2 = (0, 1, 0)
positions[9] = 1.0f;  positions[10] = 1.0f;   // v3 = (1, 1, 0)
int32[] indices = heap int32[6];
indices[1] = 1;  indices[2] = 2;          // (0, 1, 2)
indices[3] = 1;  indices[4] = 3;  indices[5] = 2;   // (1, 3, 2)
float32[] quadrics #= MeshSimplifier.accumulateQuadrics(positions, indices, 4);
int32[] survivors #= MeshSimplifier.simplify(positions, indices, 4, 1);
```

## Methods

| Signature | |
|---|---|
| `static #float32[] accumulateQuadrics(float32[] positions, int32[] indices, int32 vertexCount)` ⚑ | Accumulate a Garland–Heckbert error quadric per vertex into a flat `float32[]` (16 row-major entries per vertex; entry `v*16 + r*4 + c`) |
| `static Matrix<float32,4,4> vertexQuadric(float32[] quadrics, int32 v)` | Reconstruct vertex `v`'s accumulated quadric as a `Matrix<float32,4,4>` from the flat store |
| `static #int32[] simplify(float32[] positions, int32[] indices, int32 vertexCount, int32 targetTriangleCount)` ⚑ | Iterative edge collapse down to at most `targetTriangleCount` triangles, returning the surviving index list (length a multiple of 3); `positions` is mutated in place |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/xpu/mesh/MeshSimplifier.cajeta`](../../../../runtime/src/cajeta/xpu/mesh/MeshSimplifier.cajeta)
