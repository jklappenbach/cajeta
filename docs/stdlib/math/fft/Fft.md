# Fft

`cajeta.math.fft.Fft` — the discrete Fourier transform surface. Complex
signals are carried as an interleaved `float32` tensor: a length-`N` complex
signal is a `Tensor<float32>` of length `2N` with `[2i]` = real and
`[2i+1]` = imaginary (the byte layout a future `complex64` dtype would use).
`fft`/`ifft` are the radix-2 Cooley-Tukey core (length a power of two);
`ifft` conjugates the twiddles and scales by `1/N`, so `ifft(fft(x)) ≈ x`.
Accumulates in `float32`, so results match a reference DFT to a float
tolerance, not bit-exact.

```cajeta
package snip.fft;

import cajeta.math.Tensor;
import cajeta.math.fft.Fft;

public final class Demo {
    public static void run() {
        int64[] shp = heap int64[1];
        shp[0] = 16;                            // 8 complex samples, interleaved
        Tensor<float32> x #= Tensor.zeros<float32>(shp);
        x.set1(0, 1.0f);                        // impulse
        Tensor<float32> spec #= Fft.fft(x);
        Tensor<float32> back #= Fft.ifft(spec);  // ≈ x
        return;
    }
}
```

## Methods

| Signature | |
|---|---|
| `static #Tensor<float32> fft(Tensor<float32> x)` ⚑ | Forward DFT of an interleaved-`float32` complex signal (length `2N`, `N` a power of two) → interleaved `float32` of the same length |
| `static #Tensor<float32> ifft(Tensor<float32> x)` ⚑ | Inverse DFT (numpy `ifft`): conjugate twiddles + `1/N` scaling, so `ifft(fft(x)) ≈ x` |
| `static #Tensor<float32> fftfreq(int64 n, float32 d)` | Sample frequencies for an `n`-point DFT with sample spacing `d` (numpy `fftfreq`): `[0, 1, …, ⌈n/2⌉-1, -⌊n/2⌋, …, -1] / (n·d)` |
| `static #Tensor<float32> fftshift(Tensor<float32> x)` | Shift the zero-frequency component to the centre of a 1-D real spectrum (numpy `fftshift`): a right roll by `⌊n/2⌋` |
| `static #Tensor<float32> rfft(Tensor<float32> real)` ⚑ | Forward DFT of a real `float32` signal of length `n` (numpy `rfft`): the non-redundant half spectrum — the first `n/2 + 1` complex bins as interleaved `float32` |
| `static #Tensor<float32> irfft(Tensor<float32> half, int64 n)` | Inverse of `rfft` (numpy `irfft`): reconstructs the `n` real samples from the half spectrum by restoring Hermitian symmetry |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/math/fft/Fft.cajeta`](../../../../runtime/src/cajeta/math/fft/Fft.cajeta)
- [Tensor](../Tensor.md) — the carrier type for signals and spectra
