<p align="center">
  <img src="docs/banner.png" alt="ingot" width="820">
</p>

<p align="center">
  <a href="https://github.com/svy04/ingot/blob/main/LICENSE"><img src="https://img.shields.io/badge/license-Apache--2.0-087ea3" alt="License Apache-2.0"></a>
  <a href="https://github.com/svy04/ingot/stargazers"><img src="https://img.shields.io/github/stars/svy04/ingot?color=087ea3&label=%E2%98%85" alt="Stars"></a>
  <img src="https://img.shields.io/badge/C99-2%2C468%20lines-087ea3" alt="2,468 lines of C99">
  <img src="https://img.shields.io/badge/deps-libc%20only-5d6e73" alt="Zero dependencies">
</p>

A lossy image codec built to win on **several axes at once** — not just compression ratio. This is **v1.7**.

[한국어](README.ko.md)

Most codecs trade one property for another. ingot picks the axes that were left empty because nobody optimized for them, and locks them into the format itself.

- **Ahead of JPEG and WebP on all three metrics** — against JPEG **−31.8%** (PSNR), **−32.9%** (SSIM), **−22.6%** (SSIMULACRA2) on standard photos; against WebP **−17.7%**, **−10.1%**, **−12.9%**. Ahead of JPEG XL on PSNR by **−13.3%**. AVIF is still ahead of us: **+14.7%** on the perceptual metric, down from +32.8% a day earlier.
- **Bit-exact** — integer math only, zero floating-point operations in the library. The same input always produces the same bytes.
- **Thread count never enters the bitstream** — any number of threads, identical bytes out. The price is real and grows as groups shrink: **2.9%** at the default 256, **29.8%** at 64, measured against one group per image (6 images, 6 quality steps). Tiling in existing standards costs 3.7–8%, so this is ahead only near the default.
- **Small** — 2,643 lines of C99 in `src/`, of which 1,978 are on the decode path. That count includes 128 lines of license headers and a 1.4 KB learned probability table; the code itself is 2,515 lines. No dependencies beyond libc.
- **Hostile input is expected** — the decoder returns error codes, never crashes. 300 corrupted files, 0 abnormal exits.

```console
$ sh build.sh
built: ./ingot

$ ./ingot enc photo.ppm photo.igt 20
896x1110  q20  -> 134960 bytes (source 2983680, 4.52%)

$ ./ingot dec photo.igt out.ppm
decoded 896x1110

$ ./ingot rt photo.ppm 20
896x1110 q20 group 256  4:4:4 |   134960 B (  4.52%) | maxdiff 121 | PSNR  24.10 dB
```

Input and output are PPM (P6, 8-bit) only. Convert other formats with ffmpeg.

## Where it stands

8 images from the AOM common test set, 6 quality steps, one machine, 2026-08-23. BD-rate uses monotone (PCHIP) interpolation. **Negative means ingot spends fewer bits at equal quality.**

| vs | PSNR | SSIM | SSIMULACRA2 |
|---|---|---|---|
| JPEG | **−31.8%** | **−32.9%** | **−22.6%** |
| WebP | **−17.7%** | **−10.1%** | **−12.9%** |
| JPEG XL | **−13.3%** | +1.1% | +15.7% |
| AVIF | +41.3% | +37.8% | +14.7% |

**The third column is why v1.1 exists.** Up to v1.0 this codec was tuned against squared error alone. Adding a perceptual metric showed that at the old setting it lost to *JPEG* on SSIMULACRA2 (+5.6%) while appearing to beat WebP and JPEG XL on PSNR. One spec constant — how much coarser high-frequency coefficients are quantized — moved it from +5.6% to **−11.3%** against JPEG, at the cost of 2.4 points of PSNR. v1.1 takes that trade.

**Two things about the comparison setup, in our disfavour and in theirs.** libwebp has no lossy 4:4:4 mode — it only accepts `yuv420p` — while ingot, JPEG, and AVIF are all measured at 4:4:4 here. So the WebP column understates WebP on material where colour detail matters; the honest reading of our WebP result is not "we compress better" but "WebP cannot enter this comparison at full colour." In the other direction, AVIF is called at `cpu-used 6`, a fast preset that costs libaom real compression. Our AVIF gap is therefore *larger* than the one printed above, not smaller.

SSIMULACRA2 is a perceptual metric designed for still images; the Python implementation is used here because upstream ships no binary. It is slow (about two seconds per image), which is why the harness reports all three metrics rather than replacing the other two.

### On our own material

12 images we actually produce — AI-generated images, UI screenshots, and game sprites, 4 each:

| vs | PSNR | SSIM | SSIMULACRA2 |
|---|---|---|---|
| JPEG | **−50.9%** | **−45.9%** | **−46.2%** |
| WebP | **−31.0%** | **−26.2%** | **−22.3%** |
| JPEG XL | **−37.8%** | **−27.6%** | +1.7% |
| AVIF | +64.2% | +69.6% | +38.8% |

On this material ingot beats JPEG and WebP on all three metrics, and JPEG XL on two of three.

### The other axes

| Axis | Status | Evidence |
|---|---|---|
| Determinism | **done** | Encoding twice gives identical bytes. 0 float ops in `src/` |
| Parallelism | **in the format; the price is larger than first measured** | Against one group per image: 256 (default) **+2.9%**, 128 +10.2%, 64 **+29.8%** BD-rate (re-measured 2026-08-23). The 1.4% printed here until 2026-08-22 was measured before arithmetic coding, which resets the probability tables at every group boundary — small groups now have too few symbols to learn from. The encoder writes each group into its own slot, so an OpenMP build parallelizes it; this machine has no OpenMP runtime |
| Simplicity | **budgeted, never benchmarked against anyone** | Decode path 1,978 lines, 2,643 total (128 of them license headers), 0 external deps, spec under 14 pages. An independent pass on 2026-08-23 found 316 lines that nothing called — a whole Golomb-Rice bit-IO file left behind when arithmetic coding replaced it — and deleting them did not change one byte of output. No competitor has been measured on this axis |
| Patent freedom | expired art only | DCT (1974), Golomb (1966), arithmetic coding (late 1970s). **No legal review yet** |
| Decode speed | **slowest of the five** | 0.249 s vs JPEG 0.083, WebP 0.087, AVIF 0.095, JXL 0.098. All include process startup. v1.7 added a boundary filter to the decoder and still came out faster than v1.6's 0.178 s — not reading a flag whose answer is fixed saved more than the filter costs. Still last by a factor of two |
| Encode speed | **slowest of the five** | 2.607 s vs JPEG 0.161, WebP 0.185, JXL 0.213, AVIF 0.310. **This entry said "faster than AVIF" until 2026-08-23, and that was wrong.** The speed table was calling libaom without `-cpu-used`, the quality table with `-cpu-used 6`; two different encoders were being timed and scored. Matched up, AVIF encodes 9.5× faster than we do and lands within 3% of our file size. We are last on both speed axes |

Chroma subsampling (4:2:0) is off by default and measures as a loss on standard photos. It costs **several dB on screenshots** — measured 4.5 to 22.8 dB depending on quality — so never turn it on for text or UI.

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
| Rate-distortion level reduction | **+0.6%p, and +3.6%p on SSIMULACRA2 (reverted)** | |
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
tools/certain_flag.py  re-counts the fact the last-flag rule rests on
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
explicit exception to the trademark clause. The NOTICE here is short — the project
name, the copyright line, and where it came from.
