# Bounded-memory image previews

`Decode(ctx, reader, width, height)` returns `*image.Gray` with origin `(0,0)`,
aspect-fit bounds, and transparency composited on white. Images are never
upscaled. Reduction samples the centers of destination pixels using nearest
neighbor; it is not an antialiased photographic resize. The reader starts at
its current position and must support seeking to the end and back.

## Streaming paths

- Non-interlaced PNG: two source scanlines plus zlib state and the destination.
  Supports grayscale, RGB, indexed color, grayscale-alpha, and RGBA; all legal
  1/2/4/8/16-bit combinations; all five filters; palette and tRNS transparency;
  split/empty consecutive IDAT chunks; CRC and zlib checksum verification.
  Palette indices are validated even in pixels discarded during reduction.
  Unknown ancillary chunks are skipped in bounded buffers. Unknown critical
  chunks, invalid ordering, truncation, extra compressed/decompressed image
  data, and missing IEND are rejected.
- Baseline 8-bit JPEG: canonical Huffman entropy decoding into one MCU (minimum
  coded unit), retaining no source-sized image or coefficient frame. Inverse
  DCT is performed only for MCUs selected by destination samples. Supports
  grayscale and three-component YCbCr/RGB, interleaved component reordering,
  restart intervals, and ordinary 4:4:4, 4:4:0, 4:2:2, 4:2:0, 4:1:1, and 4:1:0
  layouts. The fixed-point IDCT can differ slightly from other JPEG decoders.
  Tests allow two grayscale levels of difference from Go's decoder.

## Strictly small fallback

The Go standard library handles progressive/extended/uncommon JPEG, Adam7 PNG,
and the first GIF frame only when source area is at most 262,144 pixels.
JPEG additionally has a conservative 32-pixel-padded area bound, so extremely
thin progressive images cannot bypass the coefficient-memory limit. Unsupported
larger variants return an error wrapping `ErrUnsupported`, with the feature and
fallback limit explained. There is no unrestricted full-frame fallback.

CMYK and non-interleaved multi-component JPEG are examples of fallback-only
features; support on that path is exactly what the standard library supports.
Arithmetic/lossless JPEG and other image formats are not implemented. EXIF
orientation, ICC/gamma color correction, and animation playback are not applied.

## Resource contract

- Encoded input: at most 128 MiB, measured before decoding and enforced by a
  context-aware limited reader even if the underlying file subsequently grows.
- Source dimensions: positive, at most 65,535 on either axis and 100 million
  pixels. Calculations use 64-bit arithmetic before conversion/allocation.
- PNG decompressed scanlines: at most 512 MiB including filter bytes, consumed
  incrementally. Extra decompressed bytes fail rather than being drained.
- Destination request: positive, at most 4,096 on either axis and 4 Mi pixels.
- JPEG segments: at most 65,533 bytes; PNG/JPEG chunk/marker counts are limited.
- Streaming workspace is independent of source area. Maximum PNG row storage
  is about 1 MiB (two 65,535-pixel RGBA16 rows); JPEG retains one MCU plus a
  bounded marker buffer. The destination adds at most 4 MiB. Small fallback
  uses more memory, including bounded progressive coefficients.
- Cancellation is checked on source reads, PNG rows, JPEG MCUs, and fallback
  resampling rows. Cancellation cannot interrupt an arbitrary reader's blocked
  `Read` or `Seek`. Callers must close/unblock such readers themselves if needed.

## Tests

Run on the Windows host with `GOOS=windows GOARCH=amd64 CGO_ENABLED=0`:

- `go test ./internal/lowmem`
- `go test ./internal/lowmem -run=^$ -fuzz=FuzzDecode -fuzztime=20s -parallel=2`

Cross-compile with `GOOS=linux GOARCH=mipsle GOMIPS=hardfloat CGO_ENABLED=0`:

- `go build ./internal/lowmem`

Tests generate their own valid streams, including 96-million-pixel PNG and
JPEG sources without allocating full source images. Allocation assertions
measure decoder `TotalAlloc`, not merely live heap after garbage collection.
Additional tests cover all PNG depths/filters, alpha/16-bit transparency,
CRC/zlib corruption, every truncation point of small fixtures, fallback limits,
JPEG restart and sampling layouts, cancellation, and malformed-input fuzzing.
No device access, external image programs, or CGO are required.
