# ingot

![](docs/banner.png)

A lossy image codec built to win on **several axes at once** — not just compression ratio. This is **v1.3**.

[한국어](README.ko.md)

Most codecs trade one property for another. ingot picks the axes that were left empty because nobody optimized for them, and locks them into the format itself.

- **Ahead of JPEG and WebP on all three metrics** — against JPEG **−28.4%** (PSNR), **−26.3%** (SSIM), **−17.7%** (SSIMULACRA2) on standard photos; against WebP **−11.4%**, **−2.3%**, **−6.8%**. Ahead of JPEG XL on PSNR by **−6.6%**. AVIF is still ahead of us, by 33% on the perceptual metric.
- **Bit-exact** — integer math only, zero floating-point operations in the library. The same input always produces the same bytes.
- **Thread count never enters the bitstream** — any number of threads, identical bytes out. The price is real and grows as groups shrink: **2.7%** at the default 256, **29.7%** at 64, measured against one group per image (6 images, 6 quality steps). Tiling in existing standards costs 3.7–8%, so this is ahead only near the default.
- **Small** — 2,475 lines of C99 in `src/`, of which 1,631 are on the decode path. That count includes 150 lines of license headers and a 1.4 KB learned probability table; the code itself is 2,325 lines. No dependencies beyond libc.
- **Hostile input is expected** — the decoder returns error codes, never crashes. 300 corrupted files, 0 abnormal exits.

```console
$ sh build.sh
built: ./ingot

$ ./ingot enc photo.ppm photo.igt 20
896x1110  q20  -> 136694 bytes (source 2983680, 4.58%)

$ ./ingot dec photo.igt out.ppm
decoded 896x1110

$ ./ingot rt photo.ppm 20
896x1110 q20 group 256  4:4:4 |   136694 B (  4.58%) | maxdiff 117 | PSNR  24.08 dB
```

Input and output are PPM (P6, 8-bit) only. Convert other formats with ffmpeg.

## Where it stands

8 images from the AOM common test set, 6 quality steps, one machine, 2026-08-21. BD-rate uses monotone (PCHIP) interpolation. **Negative means ingot spends fewer bits at equal quality.**

| vs | PSNR | SSIM | SSIMULACRA2 |
|---|---|---|---|
| JPEG | **−28.4%** | **−26.3%** | **−17.7%** |
| WebP | **−11.4%** | **−2.3%** | **−6.8%** |
| JPEG XL | **−6.6%** | +11.0% | +23.7% |
| AVIF | +59.1% | +59.9% | +32.8% |

**The third column is why v1.1 exists.** Up to v1.0 this codec was tuned against squared error alone. Adding a perceptual metric showed that at the old setting it lost to *JPEG* on SSIMULACRA2 (+5.6%) while appearing to beat WebP and JPEG XL on PSNR. One spec constant — how much coarser high-frequency coefficients are quantized — moved it from +5.6% to **−11.3%** against JPEG, at the cost of 2.4 points of PSNR. v1.1 takes that trade.

**Two things about the comparison setup, in our disfavour and in theirs.** libwebp has no lossy 4:4:4 mode — it only accepts `yuv420p` — while ingot, JPEG, and AVIF are all measured at 4:4:4 here. So the WebP column understates WebP on material where colour detail matters; the honest reading of our WebP result is not "we compress better" but "WebP cannot enter this comparison at full colour." In the other direction, AVIF is called at `cpu-used 6`, a fast preset that costs libaom real compression. Our AVIF gap is therefore *smaller* than the one printed above, not larger.

SSIMULACRA2 is a perceptual metric designed for still images; the Python implementation is used here because upstream ships no binary. It is slow (about two seconds per image), which is why the harness reports all three metrics rather than replacing the other two.

### On our own material

12 images we actually produce — AI-generated images, UI screenshots, and game sprites, 4 each:

| vs | PSNR | SSIM | SSIMULACRA2 |
|---|---|---|---|
| JPEG | **−41.8%** | **−34.7%** | **−32.3%** |
| WebP | **−17.3%** | **−14.2%** | **−2.7%** |
| JPEG XL | **−26.4%** | **−14.0%** | +29.5% |
| AVIF | +96.7% | +104.8% | +70.7% |

On this material ingot beats JPEG and WebP on all three metrics, and JPEG XL on two of three.

### The other axes

| Axis | Status | Evidence |
|---|---|---|
| Determinism | **done** | Encoding twice gives identical bytes. 0 float ops in `src/` |
| Parallelism | **in the format; the price is larger than first measured** | Against one group per image: 256 (default) **+2.7%**, 128 +9.8%, 64 **+29.7%** BD-rate. The 1.4% printed here until 2026-08-22 was measured before arithmetic coding, which resets the probability tables at every group boundary — small groups now have too few symbols to learn from. The encoder writes each group into its own slot, so an OpenMP build parallelizes it; this machine has no OpenMP runtime |
| Simplicity | **budgeted, never benchmarked against anyone** | Decode path 1,631 lines, 2,475 total (150 of them license headers), 0 external deps, spec under 12 pages. No competitor has been measured on this axis |
| Patent freedom | expired art only | DCT (1974), Golomb (1966), arithmetic coding (late 1970s). **No legal review yet** |
| Decode speed | **slowest of the five, and got slower** | Wall clock 0.178 s. Process startup alone is 0.059 s for ffmpeg and 0.028 s for our CLI; subtract it and the actual decode is 0.150 s vs JPEG 0.006, WebP 0.017, JXL 0.029, AVIF 0.041. The 2D context and the extra coefficient flags cost 0.08 s. This axis is going the wrong way and is not being paid for |
| Encode speed | **behind** | 1.64 s vs JPEG 0.081, WebP 0.155, JXL 0.190. Faster than AVIF (6.49 s). The 2D coefficient context added 0.5 s |

Chroma subsampling (4:2:0) is off by default and measures as a loss on standard photos (PSNR +1.4% vs +0.1% against JXL). It costs **8.5 dB on screenshots**, so never turn it on for text or UI.

## What is in the format, and what it was worth

Each change was added one at a time and kept only if it measured better. Reverted experiments are listed too, because the reason they failed is part of the design.

| Change | PSNR BD-rate | SSIM BD-rate |
|---|---|---|
| Intra prediction, 4 modes | −34.4% | −21.0% |
| Variable block size, rate-distortion choice | ≈ −8% (crossed JPEG) | |
| Binary arithmetic coding (range coder) | −1.0% → **−16.7%** | +7.9% → **−9.9%** |
| Neighbor magnitude in the coefficient context | −16.7% → **−17.6%** | −9.9% → **−10.9%** |
| Quantization dead zone (round at 5/16, not 1/2) | −17.6% → **−25.1%** | −10.9% → **−19.8%** |
| 4×4 blocks (16 → 8 → 4 recursive split) | −25.1% → −24.2% | +4.4% → **+2.7%** vs WebP |
| Per-size context for the block header | −24.2% → **−24.6%** | −19.7% → **−20.1%** |
| Split early-exit in the encoder | −24.6% → **−25.6%**, and 1.6× faster | −20.1% → **−21.0%** |
| Per-size context for coefficients | −25.6% → **−25.7%** | unchanged |
| High-frequency quantization weight (v1.1) | −25.7% → −23.3% | SSIMULACRA2 **+5.6% → −11.3%** |
| Run-length of zero coefficients | **+6.2% (reverted)** | **+6.6% (reverted)** |
| 32×32 blocks | **+13.2% (reverted)** | |
| More intra modes — tried three times | **+0.7 to +0.9%p (reverted)** | |
| Deblocking filter as post-process | **+2 to +7%p (reverted)** | |
| Coefficient tail truncation | **+4%p (reverted)** | |
| Direction-matched transform (ADST) | **+6.4%p (reverted)** | **+7.5%p (reverted)** |
| TM / Paeth in place of the plane mode | **+0.9 to +1.5%p (reverted)** | |

Three of those reverts are the same lesson. Directional modes, ADST, and loop filtering are each worth several percent inside AV1 — and each cost us several percent here. They depend on one another: ADST pays off because there are ten-plus prediction modes to select it, and many modes pay off because the coefficient model makes the mode bit cheap. Lifted out one at a time, only the cost comes along.

The full log, with the reason for each revert, is in [SPEC.md](SPEC.md).

## Design rules

1. The spec is edited before the code, never after.
2. Every change is measured on a fixed test set with three metrics — PSNR, SSIM, SSIMULACRA2 — before it is kept.
3. Reserved bits mean *reject if set*, so old decoders never guess at new files.
4. The decoder returns error codes. It does not abort, and it does not read past the buffer.

## Layout

```
SPEC.md            normative bitstream spec (Korean)
src/ingot.h        public C API
src/*.c            library, no dependencies beyond libc
tools/cli.c        command line front end
tools/bench.py     BD-rate harness against JPEG/WebP/AVIF/JXL, three metrics
tools/speed.py     encode and decode timing
test/run_tests.py  determinism, round-trip, golden files, corrupted input
```

## A note on language

Program output and both READMEs are English. Source comments, `SPEC.md`, and the test scripts are Korean, because that is the language this was designed in and the reasoning is worth more than the uniformity.

## License

Apache License 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

You may use ingot in a commercial product without opening your source. What the
license does require is attribution: section 4(d) says a redistribution must carry
the contents of the NOTICE file, and section 6 makes reproducing that NOTICE an
explicit exception to the trademark clause. The NOTICE here is two lines — the
project name and where it came from.
