# ingot

![](docs/banner.png)

A lossy image codec built to win on **several axes at once** — not just compression ratio.

[한국어](README.ko.md)

Most codecs trade one property for another. ingot picks the axes that were left empty because nobody optimized for them, and locks them into the format itself.

- **Ahead of JPEG, WebP, and JPEG XL on PSNR** — BD-rate **−25.6%**, **−9.0%**, and **−3.2%** against them on standard photos. AVIF is still ahead of us by 75%.
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

Numbers are from 8 images of the AOM common test set, 6 quality steps, one machine, 2026-08-21. BD-rate is computed with monotone (PCHIP) interpolation; negative means ingot spends fewer bits at equal quality.

| Axis | Status | Evidence |
|---|---|---|
| Compression ratio | **past JPEG, WebP, JPEG XL on PSNR** | PSNR — JPEG **−25.6%**, WebP **−9.0%**, JXL **−3.2%**, AVIF +68.6%<br>SSIM — JPEG **−21.0%**, WebP +2.4%, JXL +19.5%, AVIF +74.8% |
| Determinism | **done** | Encoding twice gives identical bytes. 0 float ops in `src/` |
| Parallelism | **done in the format, unmeasured in wall clock** | Group size 64 → 1024 changes file size by 1.4%, quality unchanged. The encoder writes each group into its own slot, so an OpenMP build parallelizes it — not verified here |
| Simplicity | **budgeted** | Decoder path 880 lines, 0 external deps, spec under 12 pages |
| Patent freedom | expired art only | DCT (1974), Golomb (1966), arithmetic coding (late 1970s). **No legal review yet** |
| Decode speed | **competitive** | 0.096 s vs JPEG 0.065, WebP 0.076, JXL 0.088, AVIF 0.100 (6 images, process startup included) |
| Encode speed | **behind, and it is the current weak point** | 1.16 s vs JPEG 0.081, WebP 0.155, JXL 0.190. Faster than AVIF (6.49 s). Two-stage mode selection and split early-exit cut it from 2.67 s; groups are laid out for parallel encoding but this machine has no OpenMP runtime, so **parallel speed is unmeasured** |
| Generality | partially measured | Photos, screenshots, AI-generated images, game sprites |

### On our own material

Measured the same way on 12 images we actually produce — AI-generated images, UI screenshots, and game sprites, 4 each:

| vs | PSNR BD-rate | SSIM BD-rate |
|---|---|---|
| JPEG | **−39.5%** | **−27.2%** |
| WebP | **−15.5%** | +6.0% (n=11) |
| JPEG XL | **−23.1%** | **−1.9%** |
| AVIF | +103.6% | +125.7% |

On this material ingot is ahead of JPEG and JPEG XL on *both* metrics, and ahead of WebP on PSNR. On standard photos it is ahead on PSNR but still behind WebP (+2.4%) and JXL (+19.5%) on SSIM — the two metrics disagree, and that disagreement is reported rather than resolved by picking the flattering one.

Chroma subsampling (4:2:0) is off by default. It now measures as a loss on standard photos (PSNR +1.4% vs −3.2% against JXL). It stays as an option, but it costs **8.5 dB on screenshots**, so never turn it on for text or UI.

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
| Split early-exit in the encoder | −24.6% → **−25.6%**, and 1.6× faster | −20.1% → **−21.0%** |
| Per-size context for the block header | −24.2% → **−24.6%** | −19.7% → **−20.1%** |
| Run-length of zero coefficients | **+6.2% (reverted)** | **+6.6% (reverted)** |
| 32×32 blocks | **+13.2% (reverted)** | |
| More intra modes — tried three times | **+0.7 to +0.9%p (reverted)** | |
| Deblocking filter as post-process | **+2 to +7%p (reverted)** | |
| Coefficient tail truncation | **+4%p (reverted)** | |

The full log, with the reason for each revert, is in [SPEC.md](SPEC.md).

## Design rules

1. The spec is edited before the code, never after.
2. Every change is measured on a fixed test set with two metrics before it is kept.
3. Reserved bits mean *reject if set*, so old decoders never guess at new files.
4. The decoder returns error codes. It does not abort, and it does not read past the buffer.

## Layout

```
SPEC.md            normative bitstream spec (Korean)
src/ingot.h        public C API
src/*.c            library, no dependencies beyond libc
tools/cli.c        command line front end
tools/bench.py     BD-rate harness against JPEG/WebP/AVIF/JXL
tools/speed.py     encode and decode timing
test/run_tests.py  determinism, round-trip, golden files, corrupted input
```

## A note on language

Program output and both READMEs are English. Source comments, `SPEC.md`, and the test scripts are Korean, because that is the language this was designed in and the reasoning is worth more than the uniformity.

## License

BSD 2-Clause. See [LICENSE](LICENSE).
