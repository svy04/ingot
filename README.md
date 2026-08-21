# ingot

![](docs/banner.png)

A lossy image codec built to win on **several axes at once** — not just compression ratio. This is **v1**: the format is frozen, and the numbers below are what froze it.

[한국어](README.ko.md)

Most codecs trade one property for another. ingot picks the axes that were left empty because nobody optimized for them, and locks them into the format itself.

- **Ahead of JPEG, WebP, and JPEG XL on PSNR** — BD-rate **−25.7%**, **−9.1%**, and **−3.3%** on standard photos. On a perceptual metric it is a different story, reported below rather than hidden.
- **Bit-exact** — integer math only, zero floating-point operations in the library. The same input always produces the same bytes.
- **Thread count never enters the bitstream** — splitting an image into 16× more parallel groups costs **1.4%**. Tiling in existing standards costs 3.7–8%.
- **Small** — about 1,900 lines of C99 in `src/`, decoder path 880. No dependencies beyond libc.
- **Hostile input is expected** — the decoder returns error codes, never crashes. 300 corrupted files, 0 abnormal exits.

```console
$ sh build.sh
built: ./ingot

$ ./ingot enc photo.ppm photo.igt 20
896x1110  q20  -> 221719 bytes (source 2983680, 7.43%)

$ ./ingot dec photo.igt out.ppm
decoded 896x1110

$ ./ingot rt photo.ppm 20
896x1110 q20 group 256  4:4:4 |   221719 B (  7.43%) | maxdiff 106 | PSNR  27.86 dB
```

Input and output are PPM (P6, 8-bit) only. Convert other formats with ffmpeg.

## Where it stands

8 images from the AOM common test set, 6 quality steps, one machine, 2026-08-21. BD-rate uses monotone (PCHIP) interpolation. **Negative means ingot spends fewer bits at equal quality.**

| vs | PSNR | SSIM | VMAF |
|---|---|---|---|
| JPEG | **−25.7%** | **−21.0%** | **−3.2%** |
| WebP | **−9.1%** | +2.2% | +33.9% |
| JPEG XL | **−3.3%** | +19.4% | +42.4% |
| AVIF | +68.4% | +74.7% | +72.6% |

**Read the third column before believing the first.** ingot was tuned against squared error, so it wins on PSNR and loses on a perceptual metric. Against WebP the same codec is 9% ahead by PSNR and 34% behind by VMAF. That gap is the honest description of where this codec is, and closing it is the next piece of work — not a footnote.

VMAF is used because SSIMULACRA2 ships no binary and building it pulls in all of libjxl. VMAF was trained on video, not stills, so it is a third axis rather than a replacement for the other two.

### On our own material

12 images we actually produce — AI-generated images, UI screenshots, and game sprites, 4 each:

| vs | PSNR | SSIM | VMAF |
|---|---|---|---|
| JPEG | **−39.9%** | **−27.7%** | **−39.1%** |
| WebP | **−15.8%** | +4.9% (n=11) | +28.8% |
| JPEG XL | **−23.5%** | **−2.7%** | +8.4% |
| AVIF | +102.5% | +124.3% | +92.8% |

On this material ingot beats JPEG on all three metrics and JPEG XL on two of three.

### The other axes

| Axis | Status | Evidence |
|---|---|---|
| Determinism | **done** | Encoding twice gives identical bytes. 0 float ops in `src/` |
| Parallelism | **in the format; wall clock unmeasured** | Group size 64 → 1024 changes file size by 1.4%. The encoder writes each group into its own slot, so an OpenMP build parallelizes it — this machine has no OpenMP runtime |
| Simplicity | **budgeted** | Decoder path 880 lines, 0 external deps, spec under 12 pages |
| Patent freedom | expired art only | DCT (1974), Golomb (1966), arithmetic coding (late 1970s). **No legal review yet** |
| Decode speed | **competitive** | 0.096 s vs JPEG 0.065, WebP 0.076, JXL 0.088, AVIF 0.100 (6 images, process startup included) |
| Encode speed | **behind** | 1.16 s vs JPEG 0.081, WebP 0.155, JXL 0.190. Faster than AVIF (6.49 s) |

Chroma subsampling (4:2:0) is off by default and measures as a loss on standard photos (PSNR +1.4% vs −3.3% against JXL). It costs **8.5 dB on screenshots**, so never turn it on for text or UI.

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
2. Every change is measured on a fixed test set with three metrics before it is kept.
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

BSD 2-Clause. See [LICENSE](LICENSE).
