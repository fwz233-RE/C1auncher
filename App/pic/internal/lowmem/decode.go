// Package lowmem decodes images to bounded-size grayscale previews without
// allocating source-sized images for baseline JPEG or non-interlaced PNG.
package lowmem

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"image"
	"image/gif"
	"image/jpeg"
	"image/png"
	"io"
)

// Limits are intentionally fixed: callers cannot accidentally request a
// source-sized allocation on the approximately 50 MB target device.
const (
	MaxDimension             = 65535
	MaxSourcePixels    int64 = 100_000_000
	MaxFileBytes       int64 = 128 << 20
	MaxDecodedBytes    int64 = 512 << 20
	MaxOutputDimension       = 4096
	MaxOutputPixels    int64 = 4 << 20
	// SmallFallbackPixels bounds standard-library allocations, including JPEG
	// progressive coefficient buffers. GIF returns only its first frame.
	SmallFallbackPixels int64 = 262144
)

var (
	ErrLimit       = errors.New("lowmem: image exceeds safety limit")
	ErrUnsupported = errors.New("lowmem: unsupported image feature")
)

// Decode reads an image from r's current position and returns an origin-zero,
// aspect-fit grayscale image, compositing transparency on white. It does not
// enlarge the source. Resampling is nearest-neighbor at pixel centers; it does
// not provide antialiased reduction. Non-interlaced PNG and single-scan baseline
// JPEG use bounded streaming storage. Uncommon JPEG, interlaced PNG, and GIF use
// the standard library only below SmallFallbackPixels. EXIF orientation and
// color profiles are not applied. The caller owns r and must serialize access.
// Cancellation is checked between reads, rows, and JPEG MCUs; it cannot
// interrupt a Read or Seek that blocks inside the supplied reader.
func Decode(ctx context.Context, r io.ReadSeeker, width, height int) (*image.Gray, error) {
	if ctx == nil || r == nil {
		return nil, errors.New("lowmem: nil context or reader")
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	if width < 1 || height < 1 || width > MaxOutputDimension || height > MaxOutputDimension || int64(width)*int64(height) > MaxOutputPixels {
		return nil, fmt.Errorf("%w: output bounds", ErrLimit)
	}
	start, err := r.Seek(0, io.SeekCurrent)
	if err != nil {
		return nil, err
	}
	end, err := r.Seek(0, io.SeekEnd)
	if err != nil {
		return nil, err
	}
	if _, err = r.Seek(start, io.SeekStart); err != nil {
		return nil, err
	}
	if start < 0 || end < start || end-start > MaxFileBytes {
		return nil, fmt.Errorf("%w: encoded file (maximum %d bytes)", ErrLimit, MaxFileBytes)
	}
	newReader := func() (*bufio.Reader, error) {
		if _, err := r.Seek(start, io.SeekStart); err != nil {
			return nil, err
		}
		return bufio.NewReaderSize(&checkedReader{ctx: ctx, r: r, remaining: end - start}, 4096), nil
	}
	br, err := newReader()
	if err != nil {
		return nil, err
	}
	magic, err := br.Peek(2)
	if err != nil {
		return nil, err
	}
	var format string
	switch {
	case magic[0] == 0xff && magic[1] == 0xd8:
		format = "jpeg"
	case magic[0] == 0x89 && magic[1] == 'P':
		format = "png"
	case magic[0] == 'G' && magic[1] == 'I':
		format = "gif"
	default:
		return nil, fmt.Errorf("%w: expected JPEG, PNG, or GIF", ErrUnsupported)
	}
	var out *image.Gray
	switch format {
	case "jpeg":
		out, err = decodeJPEG(ctx, br, width, height)
	case "png":
		out, err = decodePNG(ctx, br, width, height)
	case "gif":
		err = fmt.Errorf("%w: GIF requires small-image fallback", ErrUnsupported)
	}
	if err == nil {
		return out, ctx.Err()
	}
	if !errors.Is(err, ErrUnsupported) {
		return nil, err
	}
	unsupported := err
	br, err = newReader()
	if err != nil {
		return nil, err
	}
	var cfg image.Config
	switch format {
	case "jpeg":
		cfg, err = jpeg.DecodeConfig(br)
	case "png":
		cfg, err = png.DecodeConfig(br)
	case "gif":
		cfg, err = gif.DecodeConfig(br)
	}
	if err != nil {
		return nil, err
	}
	if err = sourceLimits(cfg.Width, cfg.Height); err != nil {
		return nil, err
	}
	if int64(cfg.Width)*int64(cfg.Height) > SmallFallbackPixels {
		return nil, fmt.Errorf("%w; small-image fallback limited to %d pixels (got %dx%d)", unsupported, SmallFallbackPixels, cfg.Width, cfg.Height)
	}
	// Progressive coefficient storage is MCU-padded. A pixel-only check is
	// insufficient for extremely thin images (for example 1 x 65535). Bound
	// the worst-case padded area as well, before calling the standard decoder.
	if format == "jpeg" {
		padded := ((int64(cfg.Width) + 31) / 32 * 32) * ((int64(cfg.Height) + 31) / 32 * 32)
		if padded > SmallFallbackPixels {
			return nil, fmt.Errorf("%w; JPEG padded fallback area exceeds %d pixels", unsupported, SmallFallbackPixels)
		}
	}
	br, err = newReader()
	if err != nil {
		return nil, err
	}
	var src image.Image
	switch format {
	case "jpeg":
		src, err = jpeg.Decode(br)
	case "png":
		src, err = png.Decode(br)
	case "gif":
		src, err = gif.Decode(br)
	}
	if err != nil {
		return nil, err
	}
	if src.Bounds().Dx() != cfg.Width || src.Bounds().Dy() != cfg.Height {
		return nil, errors.New("lowmem: decoded dimensions changed")
	}
	ow, oh := fit(cfg.Width, cfg.Height, width, height)
	out = image.NewGray(image.Rect(0, 0, ow, oh))
	for y := 0; y < oh; y++ {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		sy := sample(y, cfg.Height, oh) + src.Bounds().Min.Y
		for x := 0; x < ow; x++ {
			r, g, b, a := src.At(sample(x, cfg.Width, ow)+src.Bounds().Min.X, sy).RGBA()
			// RGBA returns premultiplied 16-bit channels.
			gray := (19595*uint64(r) + 38470*uint64(g) + 7471*uint64(b) + 32768) >> 16
			out.Pix[y*out.Stride+x] = uint8((gray + 65535 - uint64(a) + 128) / 257)
		}
	}
	return out, ctx.Err()
}

type checkedReader struct {
	ctx       context.Context
	r         io.Reader
	remaining int64
}

func (r *checkedReader) Read(p []byte) (int, error) {
	if err := r.ctx.Err(); err != nil {
		return 0, err
	}
	if len(p) == 0 {
		return 0, nil
	}
	if r.remaining <= 0 {
		return 0, io.EOF
	}
	if int64(len(p)) > r.remaining {
		p = p[:int(r.remaining)]
	}
	n, err := r.r.Read(p)
	r.remaining -= int64(n)
	if n == 0 && err == nil {
		return 0, io.ErrNoProgress
	}
	return n, err
}
func sourceLimits(w, h int) error {
	if w < 1 || h < 1 || w > MaxDimension || h > MaxDimension || int64(w)*int64(h) > MaxSourcePixels {
		return fmt.Errorf("%w: source dimensions %dx%d", ErrLimit, w, h)
	}
	return nil
}
func fit(w, h, mw, mh int) (int, int) {
	if w <= mw && h <= mh {
		return w, h
	}
	if int64(w)*int64(mh) > int64(h)*int64(mw) {
		return mw, max(1, int(int64(h)*int64(mw)/int64(w)))
	}
	return max(1, int(int64(w)*int64(mh)/int64(h))), mh
}
func sample(p, source, dest int) int {
	return int((2*int64(p) + 1) * int64(source) / (2 * int64(dest)))
}
func grayWhite(r, g, b, a uint32) uint8 {
	gray := (19595*uint64(r) + 38470*uint64(g) + 7471*uint64(b) + 32768) >> 16
	gray = (gray*uint64(a) + 65535*(65535-uint64(a)) + 32767) / 65535
	return uint8((gray + 128) / 257)
}
