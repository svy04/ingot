# ingot

A lossy image codec built to win on **several axes at once** — not just compression ratio.

[한국어](README.ko.md)

Most codecs trade one property for another. ingot picks the axes that were left empty because nobody optimized for them, and locks them into the format itself.

- **Bit-exact** — integer math only, zero floating-point operations in the library. The same input always produces the same bytes.
- **Thread count never enters the bitstream** — splitting an image into 16× more parallel groups costs **1.4%**. Tiling in existing standards costs 3.7–8%.
- **Small** — about 1,300 lines of C99 in `src/`. No dependencies beyond libc.
- **Hostile input is expected** — the decoder returns error codes, never crashes. 300 corrupted files, 0 abnormal exits.
- **Roughly at JPEG parity** — with chroma subsampling on, ingot is **1.3% behind JPEG** at equal PSNR. Still behind WebP (+15%), JPEG XL (+34%), and AVIF (+159%).

```console
$ sh build.sh
built: ./ingot

$ ./ingot enc photo.ppm photo.igt 20
1404x936  q20  -> 362219 bytes (source 3941568, 9.19%)

$ ./ingot dec photo.igt out.ppm
decoded 1404x936

$ ./ingot rt photo.ppm 20
1404x936 q20 group 256  4:4:4 |   362219 B (  9.19%) | maxdiff  22 | PSNR  38.64 dB
```

Input and output are PPM (P6, 8-bit) only. Convert other formats with ffmpeg.

## Where it stands

Eight axes, measured. Numbers are from 8 images of the AOM common test set, 6 quality steps, one machine, 2026-08-21.

| Axis | Status | Evidence |
|---|---|---|
| Determinism | **done** | Encoding twice gives identical bytes. 0 float ops in `src/` |
| Parallelism | **done** | Group size 64 → 1024 changes file size by 1.4%, quality unchanged |
| Simplicity | **budgeted** | Decoder core under 1,500 lines, 0 external deps, spec under 10 pages |
| Patent freedom | expired art only | DCT (1974), Golomb (1966), Rice (1979). **No legal review yet** |
| Compression ratio | **JPEG parity** | 4:4:4 — JPEG +7.4%, WebP +20.1%, JXL +39.3%, AVIF +147.7%<br>4:2:0 — JPEG **+1.3%**, WebP +15.2%, JXL +33.5%, AVIF +159.3% |
| Encode speed | not claimed | Currently faster, but only because it compresses less |
| Decode speed | not measured | |
| Generality | partially measured | Photos, screenshots, AI-generated images, game sprites |

Chroma subsampling (4:2:0) is off by default. Turning it on saves 10.2% (PSNR) / 13.2% (SSIM) on photos, but costs **8.5 dB on screenshots** — colored text edges collapse. Pass `1` as the fifth argument to enable it.

## Why the ratio is behind, on purpose

Four things every mature codec has are deliberately absent, so that each one can be measured on its own instead of trusted from a paper:

1. ~~Intra prediction~~ — **added 2026-08-21.** Four modes (DC, vertical, horizontal, plane), 2 bits per block, encoder reconstructs like the decoder. Worth −34.4% PSNR.
2. **Perceptual quantization weighting** — measured at 0 gain in this codec so far; see the note below.
3. **Larger and variable block sizes** — fixed 8×8.
4. **Arithmetic coding** — adaptive Golomb-Rice instead, which is simpler to debug.

What has been measured so far, each added one at a time:

| Change | PSNR BD-rate | SSIM BD-rate |
|---|---|---|
| DC prediction | −9.4% | — |
| Position/plane quantization weighting | −0.6% | −1.5% |
| Adaptive Golomb-Rice (8 contexts) | cumulative −11.7% | cumulative −10.4% |
| Run-length of zero coefficients | **+6.2% (reverted)** | **+6.6% (reverted)** |
| Chroma subsampling (optional) | −10.2% | −13.2% |
| Intra prediction, 4 modes | **−34.4%** | **−21.0%** |
| Adaptive coding of the block header | −49% file size at q40, −58% at q63 | |

Two findings worth recording. **Run-length coding made things worse** — low-frequency coefficients are rarely zero, so "skipped zeros = 0" costs an extra bit on almost every nonzero value. And **high-frequency quantization weighting measured as pure loss under both PSNR and SSIM**, which is why it is set to 0; JPEG's quantization tables were tuned against human viewers, not against these metrics.

## Design rules

- **SPEC.md changes before the code does.** If the golden files break, the format changed.
- No floating point in the library. The CLI's quality math is the only exception.
- Reserved bits mean **"reject if set"**, not "ignore". Otherwise old decoders silently misrender new files.
- Never judge a change by one metric. PSNR and SSIM disagree often, and the disagreement is information.

## Layout

```
SPEC.md            bitstream format — the source of truth
src/ingot.h        public C API
src/transform.c    integer 8×8 DCT (the table is part of the spec)
src/color.c        RGB↔YCbCr, fixed point
src/bitio.c        bit I/O, adaptive Golomb-Rice
src/encode.c       encoder
src/decode.c       decoder — written as a hostile parser
tools/cli.c        command line tool
tools/bench.py     BD-rate harness (PSNR + SSIM, monotone interpolation)
tools/compare.py   side-by-side against JPEG/WebP/AVIF/JPEG XL
test/run_tests.py  determinism, round-trip, golden files, corrupted input
```

Test images are not in this repository. Get the AOM common test set from
`media.xiph.org/video/aomctc/test_set/` and convert to PPM with ffmpeg.

## A note on language

The library and the `ingot` CLI speak English. **Source comments, `SPEC.md`, and the developer scripts under `tools/` and `test/` are in Korean** — the comments carry the reasoning behind each decision, and translating them would flatten it. An English spec is planned; until then the code and the golden test vectors are the normative reference.

## License

BSD 2-Clause. See [LICENSE](LICENSE).
