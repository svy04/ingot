<p align="center">
  <img src="docs/banner.png" alt="ingot" width="820">
</p>

A lossy image codec built to win on **several axes at once** — not just compression ratio. This is **v1.7**.

[한국어](README.ko.md)

Most codecs trade one property for another. ingot picks the axes that were left empty because nobody optimized for them, and locks them into the format itself.

- **Ahead of JPEG and WebP on all three metrics** — against JPEG **−31.6%** (PSNR), **−30.8%** (SSIM), **−21.8%** (SSIMULACRA2) on standard photos; against WebP **−14.5%**, **−6.2%**, **−11.0%**. Ahead of JPEG XL on PSNR by **−10.9%**. AVIF is still ahead of us: **+18.2%** on the perceptual metric, down from +32.8% a day earlier.
- **Bit-exact** — integer math only, zero floating-point operations in the library. The same input always produces the same bytes.
- **Thread count never enters the bitstream** — any number of threads, identical bytes out. The price is real and grows as groups shrink: **2.7%** at the default 256, **29.7%** at 64, measured against one group per image (6 images, 6 quality steps). Tiling in existing standards costs 3.7–8%, so this is ahead only near the default.
- **Small** — 2,468 lines of C99 in `src/`, of which 1,832 are on the decode path. That count includes 128 lines of license headers and a 1.4 KB learned probability table; the code itself is 2,340 lines. No dependencies beyond libc.
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

8 images from the AOM common test set, 6 quality steps, one machine, 2026-08-23. BD-rate uses monotone (PCHIP) interpolation. **Negative means ingot spends fewer bits at equal quality.**

| vs | PSNR | SSIM | SSIMULACRA2 |
|---|---|---|---|
| JPEG | **−31.6%** | **−30.8%** | **−21.8%** |
| WebP | **−14.5%** | **−6.2%** | **−11.0%** |
| JPEG XL | **−10.9%** | +5.3% | +17.7% |
| AVIF | +45.8% | +42.2% | +18.2% |

**The third column is why v1.1 exists.** Up to v1.0 this codec was tuned against squared error alone. Adding a perceptual metric showed that at the old setting it lost to *JPEG* on SSIMULACRA2 (+5.6%) while appearing to beat WebP and JPEG XL on PSNR. One spec constant — how much coarser high-frequency coefficients are quantized — moved it from +5.6% to **−11.3%** against JPEG, at the cost of 2.4 points of PSNR. v1.1 takes that trade.

**Two things about the comparison setup, in our disfavour and in theirs.** libwebp has no lossy 4:4:4 mode — it only accepts `yuv420p` — while ingot, JPEG, and AVIF are all measured at 4:4:4 here. So the WebP column understates WebP on material where colour detail matters; the honest reading of our WebP result is not "we compress better" but "WebP cannot enter this comparison at full colour." In the other direction, AVIF is called at `cpu-used 6`, a fast preset that costs libaom real compression. Our AVIF gap is therefore *smaller* than the one printed above, not larger.

SSIMULACRA2 is a perceptual metric designed for still images; the Python implementation is used here because upstream ships no binary. It is slow (about two seconds per image), which is why the harness reports all three metrics rather than replacing the other two.

### On our own material

12 images we actually produce — AI-generated images, UI screenshots, and game sprites, 4 each:

| vs | PSNR | SSIM | SSIMULACRA2 |
|---|---|---|---|
| JPEG | **−45.3%** | **−37.8%** | **−36.7%** |
| WebP | **−22.3%** | **−17.4%** | **−8.3%** |
| JPEG XL | **−30.5%** | **−17.8%** | +22.2% |
| AVIF | +82.8% | +93.3% | +56.2% |

On this material ingot beats JPEG and WebP on all three metrics, and JPEG XL on two of three.

### The other axes

| Axis | Status | Evidence |
|---|---|---|
| Determinism | **done** | Encoding twice gives identical bytes. 0 float ops in `src/` |
| Parallelism | **in the format; the price is larger than first measured** | Against one group per image: 256 (default) **+2.9%**, 128 +10.2%, 64 **+29.8%** BD-rate (re-measured 2026-08-23). The 1.4% printed here until 2026-08-22 was measured before arithmetic coding, which resets the probability tables at every group boundary — small groups now have too few symbols to learn from. The encoder writes each group into its own slot, so an OpenMP build parallelizes it; this machine has no OpenMP runtime |
| Simplicity | **budgeted, never benchmarked against anyone** | Decode path 1,832 lines, 2,468 total (128 of them license headers), 0 external deps, spec under 14 pages. An independent pass on 2026-08-23 found 316 lines that nothing called — a whole Golomb-Rice bit-IO file left behind when arithmetic coding replaced it — and deleting them did not change one byte of output. No competitor has been measured on this axis |
| Patent freedom | expired art only | DCT (1974), Golomb (1966), arithmetic coding (late 1970s). **No legal review yet** |
| Decode speed | **slowest of the five, but no longer getting worse** | Wall clock 0.171 s vs JPEG 0.063, WebP 0.079, JXL 0.088, AVIF 0.085. Both numbers include process startup. v1.7 added a boundary filter to the decoder and still came out *faster* than v1.6's 0.178 s — not reading a flag whose answer is fixed saved more than the filter costs. Still last by a factor of two |
| Encode speed | **behind everything but AVIF** | 2.74 s vs JPEG 0.083, WebP 0.157, JXL 0.234; AVIF is 6.41 s. v1.7 went from 1.64 s because the encoder now tries all four prediction modes instead of the top two by residual sum. That trade bought −2.1% BD-rate and is a compile-time knob (`INGOT_MODE_TRIALS`) |

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
| Pricing the mode symbol per candidate (v1.7, encoder only) | **−0.7%** | −0.4% |
| Trying all four modes instead of the top two (v1.7, encoder only) | **−2.1%** | −1.7% |
| Not coding a flag whose answer is fixed (v1.7) | **−4.9%** | **−4.3%** |
| Block boundary filter (v1.7, decoder only, costs zero bits) | **−7.2%** | **−7.7%** |
| Rate-distortion level reduction | **+0.6%p, and −3.6%p on SSIMULACRA2 (reverted)** | |
| Per-block lambda from local variance | **+7.2%p on SSIMULACRA2 (reverted)** | |
| Context model on coefficient signs | **0.55% ceiling, not taken** | |
| Skipping blocks entirely outside the image | **0.0%, not taken** | |
| Not coding modes that are indistinguishable here | **a tie, not taken** | |

**Two of those reverts were wrong, and finding out why is the most useful thing in this table.**

The deblocking filter was reverted on 2026-08-21 at 16:08. The perceptual metric was added to the harness at 23:16 the same day. So a smoothing filter — the one kind of change PSNR and SSIM are worst at seeing — was judged by PSNR and SSIM alone. Rebuilt with a flatness test and an edge test and measured against all three metrics, it is worth **−4.8 percentage points on SSIMULACRA2** and costs zero bits. It is in the format as of v1.7.

More intra modes lost three times for a different reason: the encoder was picking modes without pricing them. The mode symbol's cost was added *after* the selection loop, as the same constant for all four candidates, so the difference between a cheap mode and an expensive one was exactly zero. Trying all four modes with real prices is worth **−2.1%**; that fix has to come before any experiment that adds modes.

What survives of the original claim is ADST. It still depends on having enough directional modes for the transform choice to matter, and we still have four.

The full log, with the reason for each revert, is in [SPEC.md](SPEC.md).

## Design rules

1. The spec is edited before the code, never after.
2. Every change is measured on a fixed test set with three metrics — PSNR, SSIM, SSIMULACRA2 — before it is kept.
   A rejected result is only as good as the metrics that were running when it was taken. Two changes in the table above were rejected under a two-metric harness and had to be re-measured once the third existed.
3. Reserved bits mean *reject if set*, so old decoders never guess at new files.
4. The decoder returns error codes. It does not abort, and it does not read past the buffer.

## Layout

```
SPEC.md            normative bitstream spec (Korean)
src/ingot.h        public C API
src/*.c            library, no dependencies beyond libc
tools/cli.c        command line front end
tools/bench.py     BD-rate harness against JPEG/WebP/AVIF/JXL, three metrics
tools/trial.py     BD-rate of one build against a saved baseline of our own
tools/sweep.py     one knob, several values, one line each
tools/learn_probs.py  regenerates the normative probability table
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
