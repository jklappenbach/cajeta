# Npy

`cajeta.math.npio.Npy` — NumPy `.npy` binary array I/O. Reads and writes the
NumPy v1.0 format: magic + version + header, then the raw little-endian
element bytes in C-order. Four dtypes are supported, each a typed
encode/decode pair over in-memory byte images plus a save/load pair over
files (cajeta is statically typed, so the caller picks the variant matching
the dtype): `float32` `'<f4'`, `float64` `'<f8'`, `int32` `'<i4'`,
`int64` `'<i8'`. `save*` is `encode*` + `File.writeAllBytes`; `load*` is
`File.readAllBytes` + `decode*` — use the encode/decode forms to keep the
`.npy` image in memory (sockets, archives) instead of touching disk.

```cajeta
package snip.npy;

import cajeta.math.Tensor;
import cajeta.math.npio.Npy;

public final class Demo {
    public static void run() {
        Tensor<float32> t = Tensor.arange<float32>(6);
        Npy.saveF32("data.npy", t);
        Tensor<float32> back = Npy.loadF32("data.npy");
        return;
    }
}
```

## Methods

| Signature | |
|---|---|
| `static #int8[] encodeF32(Tensor<float32> t)` | Serialize a `float32` tensor to its full `.npy` byte image (`'<f4'`) |
| `static #Tensor<float32> decodeF32(int8[] buf)` | Decode a `float32` `.npy` byte image (starting at offset 0) |
| `static void saveF32(String path, Tensor<float32> t)` ⚑ | Save a `float32` tensor to `path` as a `.npy` (`'<f4'`, C-order) |
| `static #Tensor<float32> loadF32(String path)` ⚑ | Load a `float32` `.npy` from `path` |
| `static #int8[] encodeF64(Tensor<float64> t)` | Serialize a `float64` tensor to its full `.npy` byte image (`'<f8'`) |
| `static #Tensor<float64> decodeF64(int8[] buf)` | Decode a `float64` `.npy` byte image |
| `static void saveF64(String path, Tensor<float64> t)` | Save a `float64` tensor (`'<f8'`, C-order) |
| `static #Tensor<float64> loadF64(String path)` | Load a `float64` `.npy` |
| `static #int8[] encodeI32(Tensor<int32> t)` | Serialize an `int32` tensor to its full `.npy` byte image (`'<i4'`) |
| `static #Tensor<int32> decodeI32(int8[] buf)` | Decode an `int32` `.npy` byte image |
| `static void saveI32(String path, Tensor<int32> t)` | Save an `int32` tensor (`'<i4'`, C-order) |
| `static #Tensor<int32> loadI32(String path)` | Load an `int32` `.npy` |
| `static #int8[] encodeI64(Tensor<int64> t)` | Serialize an `int64` tensor to its full `.npy` byte image (`'<i8'`) |
| `static #Tensor<int64> decodeI64(int8[] buf)` | Decode an `int64` `.npy` byte image |
| `static void saveI64(String path, Tensor<int64> t)` | Save an `int64` tensor (`'<i8'`, C-order) |
| `static #Tensor<int64> loadI64(String path)` | Load an `int64` `.npy` |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/math/npio/Npy.cajeta`](../../../../runtime/src/cajeta/math/npio/Npy.cajeta)
- [Tensor](../Tensor.md) — the in-memory array type, [File](../../io/file/File.md) — the byte-level I/O underneath
