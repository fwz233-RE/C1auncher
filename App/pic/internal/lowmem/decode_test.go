package lowmem

import (
	"bytes"
	"context"
	"errors"
	"image"
	"image/color"
	"image/gif"
	"image/png"
	"io"
	"runtime"
	"strings"
	"testing"
)

func TestBoundsAndCancellation(t *testing.T) {
	var b bytes.Buffer
	src := image.NewNRGBA(image.Rect(0, 0, 31, 17))
	if err := png.Encode(&b, src); err != nil {
		t.Fatal(err)
	}
	for _, dims := range [][2]int{{0, 1}, {1, 0}, {-1, 1}, {4097, 1}, {4096, 4096}, {int(^uint(0) >> 1), 1}} {
		if _, err := Decode(context.Background(), bytes.NewReader(b.Bytes()), dims[0], dims[1]); !errors.Is(err, ErrLimit) {
			t.Fatalf("%v: %v", dims, err)
		}
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := Decode(ctx, bytes.NewReader(b.Bytes()), 30, 30); !errors.Is(err, context.Canceled) {
		t.Fatal(err)
	}
	for _, tc := range []struct{ w, h, ow, oh int }{{100, 100, 31, 17}, {10, 10, 10, 5}, {1, 100, 1, 1}, {100, 1, 1, 1}} {
		out, err := Decode(context.Background(), bytes.NewReader(b.Bytes()), tc.w, tc.h)
		if err != nil {
			t.Fatal(err)
		}
		if out.Bounds() != image.Rect(0, 0, tc.ow, tc.oh) {
			t.Fatalf("%+v: %v", tc, out.Bounds())
		}
		for _, v := range out.Pix {
			if v != 255 {
				t.Fatal("transparent source must be white")
			}
		}
	}
	// Seekable substreams start at the current position, not necessarily zero.
	prefixed := bytes.NewReader(append([]byte("prefix"), b.Bytes()...))
	prefixed.Seek(6, io.SeekStart)
	if _, err := Decode(context.Background(), prefixed, 10, 10); err != nil {
		t.Fatal(err)
	}
}

type hugeFile struct{ pos int64 }

func (r *hugeFile) Read(p []byte) (int, error) { return 0, io.EOF }
func (r *hugeFile) Seek(n int64, whence int) (int64, error) {
	switch whence {
	case io.SeekStart:
		r.pos = n
	case io.SeekCurrent:
		r.pos += n
	case io.SeekEnd:
		r.pos = MaxFileBytes + 1 + n
	}
	return r.pos, nil
}
func TestFileLimitAndUnknown(t *testing.T) {
	if _, err := Decode(context.Background(), &hugeFile{}, 10, 10); !errors.Is(err, ErrLimit) {
		t.Fatal(err)
	}
	if _, err := Decode(context.Background(), strings.NewReader("not an image"), 10, 10); !errors.Is(err, ErrUnsupported) {
		t.Fatal(err)
	}
	for _, b := range [][]byte{nil, {0xff}, {0xff, 0xd8}, {137, 'P'}} {
		if _, err := Decode(context.Background(), bytes.NewReader(b), 10, 10); err == nil {
			t.Fatalf("accepted %x", b)
		}
	}
}

type cancelReader struct {
	*bytes.Reader
	cancel    context.CancelFunc
	remaining int
}

func (r *cancelReader) Read(p []byte) (int, error) {
	n, err := r.Reader.Read(p)
	r.remaining -= n
	if r.remaining <= 0 {
		r.cancel()
	}
	return n, err
}
func TestGIFSmallFallback(t *testing.T) {
	src := image.NewPaletted(image.Rect(0, 0, 3, 2), color.Palette{color.NRGBA{0, 0, 0, 0}, color.Black})
	src.SetColorIndex(1, 0, 1)
	var b bytes.Buffer
	if err := gif.Encode(&b, src, nil); err != nil {
		t.Fatal(err)
	}
	out, err := Decode(context.Background(), bytes.NewReader(b.Bytes()), 10, 10)
	if err != nil {
		t.Fatal(err)
	}
	if out.GrayAt(0, 0).Y != 255 || out.GrayAt(1, 0).Y != 0 {
		t.Fatal(out.Pix)
	}
	large := append([]byte(nil), b.Bytes()...)
	large[6], large[7], large[8], large[9] = 0xff, 0x7f, 0xff, 0x7f
	if _, err := Decode(context.Background(), bytes.NewReader(large), 10, 10); !errors.Is(err, ErrLimit) {
		t.Fatal(err)
	}
	large[6], large[7], large[8], large[9] = 0x00, 0x04, 0x00, 0x04
	if _, err := Decode(context.Background(), bytes.NewReader(large), 10, 10); !errors.Is(err, ErrUnsupported) {
		t.Fatal(err)
	}
}

// TotalAlloc is stronger than a sampled live-heap assertion: temporary full
// frames would count even if already garbage-collected before the assertion.
func assertSmallDecodeAllocation(t *testing.T, data []byte, maxBytes uint64) {
	t.Helper()
	runtime.GC()
	var before, after runtime.MemStats
	runtime.ReadMemStats(&before)
	out, err := Decode(context.Background(), bytes.NewReader(data), 128, 128)
	runtime.ReadMemStats(&after)
	if err != nil {
		t.Fatal(err)
	}
	if out.Bounds().Dx() > 128 || out.Bounds().Dy() > 128 {
		t.Fatal(out.Bounds())
	}
	allocated := after.TotalAlloc - before.TotalAlloc
	t.Logf("decoder total allocation: %d bytes for %d encoded bytes", allocated, len(data))
	if allocated > maxBytes {
		t.Fatalf("decoder allocated %d bytes, limit %d", allocated, maxBytes)
	}
}

func FuzzDecode(f *testing.F) {
	var b bytes.Buffer
	png.Encode(&b, image.NewGray(image.Rect(0, 0, 2, 3)))
	f.Add(b.Bytes())
	f.Add(constantJPEG(17, 19, 0))
	f.Add([]byte("GIF89a"))
	f.Fuzz(func(t *testing.T, b []byte) {
		if len(b) > 1<<20 {
			t.Skip()
		}
		ctx, cancel := context.WithCancel(context.Background())
		defer cancel()
		// Keep fuzz work bounded even when random headers happen to be valid.
		_, _ = Decode(ctx, bytes.NewReader(b), 32, 32)
	})
}
