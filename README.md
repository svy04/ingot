<p align="center">
  <img src="docs/banner.png" alt="ingot" width="820">
</p>

<p align="center">
  <a href="https://github.com/svy04/ingot/blob/main/LICENSE"><img src="https://img.shields.io/badge/license-Apache--2.0-087ea3" alt="License Apache-2.0"></a>
  <a href="https://github.com/svy04/ingot/stargazers"><img src="https://img.shields.io/github/stars/svy04/ingot?color=087ea3&label=%E2%98%85" alt="Stars"></a>
  <img src="https://img.shields.io/badge/C99-5%2C364%20lines-087ea3" alt="5,364 lines of C99">
  <img src="https://img.shields.io/badge/deps-libc%20only-5d6e73" alt="Zero dependencies">
</p>

A lossy image codec built to win on **several axes at once** — not just compression ratio. This is **v1.10**.

[한국어](README.ko.md)

Most codecs trade one property for another. ingot picks the axes that were left empty because nobody optimized for them, and locks them into the format itself.

- **Ahead of JPEG, WebP and JPEG XL on all three metrics** — against JPEG **−38.8%** (PSNR), **−40.4%** (SSIM), **−33.8%** (SSIMULACRA2) on standard photos; against WebP **−25.9%**, **−18.4%**, **−24.8%**; against JPEG XL **−22.3%**, **−9.9%**, **−2.1%**. Against AVIF we are ahead on the perceptual metric by **−7.5%** and behind on the two squared-error ones by **+20.6%** and **+14.4%**.
- **Bit-exact** — integer math only, zero floating-point operations in the library. The same input always produces the same bytes.
- **Thread count never enters the bitstream** — any number of threads, identical bytes out. The price is real and grows as groups shrink: against one group per image, **1.1%** at 512, **3.3%** at 256, **20.3%** at 64 (6 images, 6 quality steps, measured 2026-08-24 on the v1.8 encoder). The default is now 1024, which is where that measurement put its zero. Tiling in existing standards costs 3.7–8%. **The v1.9 encoder has not been re-measured on this axis** — it added four prediction modes, which changes what a small group has to learn.
- **Small** — 5,364 lines of C99 in `src/`, of which 4,345 are on the decode path. Most of that is tables: the integer transform matrices up to 64×64 and a learned probability table. 620 lines sit behind knobs that are off — a second probability table, sine-transform matrices, and a restoration filter, kept because each is a measured near-miss. No dependencies beyond libc.
- **Hostile input is expected** — the decoder returns error codes, never crashes. 300 corrupted files, 0 abnormal exits.

```console
$ sh build.sh
built: ./ingot

$ ./ingot enc photo.ppm photo.igt 20
896x1110  q20  -> 134960 bytes (source 2983680, 4.52%)

$ ./ingot dec photo.igt out.ppm
decoded 896x1110

$ ./ingot rt photo.ppm 20
896x1110 q20 group 1024 4:4:4 |   134960 B (  4.52%) | maxdiff 121 | PSNR  24.10 dB
```

Input and output are PPM (P6, 8-bit) only. Convert other formats with ffmpeg.

## Where it stands

8 images from the AOM common test set, 6 quality steps, one machine, 2026-08-24. BD-rate uses monotone (PCHIP) interpolation. **Negative means ingot spends fewer bits at equal quality.**

| vs | PSNR | SSIM | SSIMULACRA2 |
|---|---|---|---|
| JPEG | **−38.8%** | **−40.4%** | **−33.8%** |
| WebP | **−25.9%** | **−18.4%** | **−24.8%** |
| JPEG XL | **−22.3%** | **−9.9%** | **−2.1%** |
| AVIF | +20.6% | +14.4% | **−7.5%** |

**AVIF is still ahead on both squared-error metrics, and the gap is 20.6%.** That is not a rounding difference — it is roughly what a mature encoder with dozens of directional prediction modes and a transform-type search buys. Measured per quality step, the gap is 1.11–1.25× at PSNR 30 and 1.3–1.96× at PSNR 40: we lose most at high quality, on flat images. The header — split flags, mode symbols, the last-coefficient position — is 13–16% of the file on those images and 4.6% on the ones where we do best.

**The third column is why v1.1 exists.** Up to v1.0 this codec was tuned against squared error alone. Adding a perceptual metric showed that at the old setting it lost to *JPEG* on SSIMULACRA2 (+5.6%) while appearing to beat WebP and JPEG XL on PSNR. One spec constant — how much coarser high-frequency coefficients are quantized — moved it from +5.6% to **−11.3%** against JPEG, at the cost of 2.4 points of PSNR. v1.1 takes that trade.

**Two things about the comparison setup, in our disfavour and in theirs.** libwebp has no lossy 4:4:4 mode — it only accepts `yuv420p` — while ingot, JPEG, and AVIF are all measured at 4:4:4 here. So the WebP column understates WebP on material where colour detail matters; the honest reading of our WebP result is not "we compress better" but "WebP cannot enter this comparison at full colour." In the other direction, AVIF is called at `cpu-used 6`, a fast preset that costs libaom real compression. Our AVIF gap is therefore *larger* than the one printed above, not smaller.

SSIMULACRA2 is a perceptual metric designed for still images; the Python implementation is used here because upstream ships no binary. It is slow (about two seconds per image), which is why the harness reports all three metrics rather than replacing the other two.

### On our own material

12 images we actually produce — AI-generated images, UI screenshots, and game sprites, 4 each:

| vs | PSNR | SSIM | SSIMULACRA2 |
|---|---|---|---|
| JPEG | **−56.3%** | **−55.6%** | **−56.9%** |
| WebP | **−38.7%** | **−42.7%** | **−37.6%** |
| JPEG XL | **−44.8%** | **−41.9%** | **−17.4%** |
| AVIF | +45.2% | +31.7% | +7.1% |

The ordering flips here. On our own material we beat JPEG XL on the perceptual
metric by 17.4% — on standard photos that margin is 1.6%. And AVIF beats us on
all three, including the perceptual one, where standard photos have us ahead by
6.5%. Screenshots and sprites have large flat regions and hard edges; whatever
we do well, we do more of it here, and so does AVIF.

### The other axes

| Axis | Status | Evidence |
|---|---|---|
| Determinism | **done** | Encoding twice gives identical bytes. 0 float ops in `src/` |
| Parallelism | **in the format; the default is now where the price is zero** | Against one group per image: 512 **+1.13%**, 256 +3.27%, 128 +8.31%, 64 **+20.29%** BD-rate (6 images, 6 quality steps, measured 2026-08-24 on the v1.8 encoder; 1024 was the reference). v1.9 moved the default to 1024 and has not been re-measured here. A 1196×1008 image is two groups at 1024 and was six at 512, so there is less to hand to threads. Probability tables still restart at every group boundary. The encoder writes each group into its own slot, so an OpenMP build parallelizes it; this machine has no OpenMP runtime |
| Simplicity | **budgeted, never benchmarked against anyone** | Decode path 4,345 lines, 5,364 total, 0 external deps, spec under 14 pages. Most of the growth since 2,643 is tables — the 64×64 transform matrix alone is 64 lines of 64 numbers. 620 of those lines sit behind knobs that are off. No competitor has been measured on this axis |
| Patent freedom | expired art only | DCT (1974), Golomb (1966), arithmetic coding (late 1970s). **No legal review yet** |
| Decode speed | **slowest of the five, by 7×** | 0.456 s vs JPEG 0.050, WebP 0.065, AVIF 0.065, JXL 0.073 (2026-08-24, 6 images, best of three, process startup included). v1.8 added 64×64 blocks and a reference-smoothing pass; decode went from 0.249 s to 0.456 s and has stayed there since |
| Encode speed | **slowest of the five, by 49×** | 11.975 s vs JPEG 0.067, WebP 0.132, JXL 0.165, AVIF 0.246. The encoder trial-encodes **all eight** prediction modes at every block size and recurses from 64×64 down to 4×4. Every compression gain in v1.8 and v1.9 was bought with encode time; the ratio was 28× one version ago. At that middle setting our file is 44,065 bytes against AVIF's 62,839 — not the same quality point, so read the two together, not either alone |

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
| Coefficient tail truncation — first attempt | **+4%p (reverted; the revert was wrong, see below)** | |
| Direction-matched transform (ADST) — first attempt | **+6.4%p (reverted)** | **+7.5%p (reverted)** |
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
| 64×64 blocks (v1.8) | **−0.60%** | **−1.13%** |
| Five neighbours in the coefficient context instead of three (v1.8) | **−0.53%** | **−0.65%** |
| Coefficient tail truncation, second attempt with the units fixed (v1.8, encoder only) | **−0.31%** | **−0.18%** |
| Smoothing the reference pixels before predicting (v1.8) | **−0.09%** | **−0.12%** |
| Deblocking strength 6 → 4, now that references are smoothed (v1.8) | **−0.09%** | **−0.07%** |
| High-frequency quantization weight 20 → 28 (v1.8) | +1.29% | +0.89%, **SSIMULACRA2 −2.01%** |
| Direction-matched transform (ADST), second attempt with a re-learned table | **+0.09% (not taken)** | **+0.23% (not taken)** |
| Emptiness of the previous block as context for the empty flag | **+0.10% (not taken)** | **+0.17% (not taken)** |
| Nine neighbour levels instead of seven | **(not taken)** | **+0.46% (not taken)** |
| A restoration filter chosen per group, signalled in spare TOC bits | **−0.40%** | **−0.83%, SSIMULACRA2 +0.28% (not taken)** |
| Lowering the high-frequency weight at high quality | **35% more bits for 0.40 dB (not taken)** | |
| Groups of 1024 instead of 512 (v1.9) | **−0.79%** | **−0.79%** |
| Eight prediction modes — four angles added, re-measured at group 1024 (v1.9) | **−0.81%** | **−0.64%** |
| Dropping the edge weight in the encoder's distortion (v1.9) | **−0.10%** | **−0.04%** |
| Eight prediction modes, measured at group 512 | **−0.26%** | +0.03%, **SSIMULACRA2 +0.63% (would have been rejected)** |
| Eleven coefficient bands instead of eight | **+0.26% (not taken)** | **+0.24% (not taken)** |
| A separate starting probability table for high quality | **−0.05 to −0.16% at q1–q4, +0.14% at q10 (not taken)** | |
| Seven magnitude flags instead of five | **(not taken)** | |
| Sixteen prediction modes — twelve angles (v1.10) | **−1.00%** | **−0.69%** |
| Thirty-two prediction modes — twenty-eight angles | **−0.32% (not taken; encode time doubles again)** | −0.09% |
| Trial-encoding only the top 12 of 16 modes | **+0.18% (not taken)** | **+0.27% (not taken)** |

**Three of those reverts were wrong, and finding out why is the most useful thing in this table.**

The deblocking filter was reverted on 2026-08-21 at 16:08. The perceptual metric was added to the harness at 23:16 the same day. So a smoothing filter — the one kind of change PSNR and SSIM are worst at seeing — was judged by PSNR and SSIM alone. Rebuilt with a flatness test and an edge test and measured against all three metrics, it is worth **−4.8 percentage points on SSIMULACRA2** and costs zero bits. It is in the format as of v1.7.

More intra modes lost three times for a different reason: the encoder was picking modes without pricing them. The mode symbol's cost was added *after* the selection loop, as the same constant for all four candidates, so the difference between a cheap mode and an expensive one was exactly zero. Trying all four modes with real prices is worth **−2.1%**; that fix has to come before any experiment that adds modes.

Tail truncation lost by **+4 percentage points** the first time. The technique was not the problem — the scale was. The encoder decides everything else with `cost = distortion * INGOT_RD_SCALE + lambda * bits`, and the truncation test used the bare distortion, dropping that 256× factor. Worse, the value it called distortion was squared error **in the coefficient domain**, not the pixel domain. Measured on random residuals, one unit of coefficient error shows up as 0.064 to 0.278 units in pixels depending on block size, because the integer transform shifts differently per size. Two missing conversions, and the cost of a bit looked hundreds of times cheaper than it was, so the encoder cut the tail off everything. With both fixed and the weight swept, it is **−0.31% / −0.18% / −0.17%** and it is in v1.8. The sign-hiding code had the same two bugs in the same file, three functions up.

ADST was re-measured with a table generated from scratch and the probability model re-learned for it. It lands at **+0.09%** — close enough that the remaining gap is probably the zigzag order or the position-based quantization weight, both of which are still DCT-shaped. The table is in `src/adst.c` behind a knob that is off.

More intra modes lost a seventh time, and then won. Four angles were added — 45°, 135°, and two between — with the mode symbol widened from a two-bit tree to three. At the default group size of 512 that was **−0.26% PSNR but +0.63% on the perceptual metric**, which would have closed the axis for the seventh time. Groups were then enlarged to 1024 for an unrelated reason, and the same code measured **−0.81% / −0.64% / −0.17%**. Probability tables restart at every group boundary; a three-bit mode tree has twice as much to learn, and at 512 there were not enough symbols in a group to learn it. **Whether a coding tool wins can depend on a parameter that has nothing to do with the tool.** That is now design rule 7.

Twelve angles then beat four by another **−1.00% / −0.69% / −0.42%**, and twenty-eight angles beat twelve by only −0.32% while doubling encode time again. The axis that lost six times in a row is now the largest single source of gain in the format, and it has reached diminishing returns three doublings later.

The full log, with the reason for each revert, is in [SPEC.md](SPEC.md).

## Design rules

1. The spec is edited before the code, never after.
2. Every change is measured on a fixed test set with three metrics — PSNR, SSIM, SSIMULACRA2 — before it is kept.
   A rejected result is only as good as the metrics that were running when it was taken. Two changes in the table above were rejected under a two-metric harness and had to be re-measured once the third existed.
3. Reserved bits mean *reject if set*, so old decoders never guess at new files.
4. The decoder returns error codes. It does not abort, and it does not read past the buffer.
5. A new decision that sits next to an existing cost function copies that function's scale and coordinate system before it copies anything else. Two of the reverts above were caused by skipping this: the cost was right, the units were not.
6. Changing the context layout means re-learning the probability table before measuring. Five experiments were rejected on this project under a table that no longer matched the layout; four of them win once the table is re-learned.
7. Anything that adds symbols or contexts is measured at more than one group size. Probability tables restart at every group boundary, so a larger alphabet needs a larger group to pay for itself. Eight prediction modes lose at group 512 and win at 1024 — same code, opposite verdict.

## Layout

```
SPEC.md            normative bitstream spec (Korean)
src/ingot.h        public C API
src/*.c            library, no dependencies beyond libc
src/adst.c         sine transform matrices, behind a knob that is off
src/probs_hi.c     a second probability table for high quality, knob off
src/restore.c      per-group restoration filter, behind a knob that is off
tools/cli.c        command line front end
tools/bench.py     BD-rate harness against JPEG/WebP/AVIF/JXL, three metrics
tools/trial.py     BD-rate of one build against a saved baseline of our own
tools/sweep.py     one knob, several values, one line each
tools/learn_probs.py  regenerates the normative probability table
tools/certain_flag.py  re-counts the fact the last-flag rule rests on
tools/gen_dct.py   regenerates the integer transform tables and zigzag orders
tools/group_cost.py  what shrinking groups costs, measured against one group
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
