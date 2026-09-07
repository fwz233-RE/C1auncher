package lowmem

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"image"
	"image/color"
	"image/jpeg"
	"testing"
)

func jpegSegment(b *bytes.Buffer, marker byte, p []byte) {
	b.Write([]byte{0xff, marker})
	binary.Write(b, binary.BigEndian, uint16(len(p)+2))
	b.Write(p)
}
func jpegConstantHeader(b *bytes.Buffer, w, h int, progressive bool) {
	b.Write([]byte{0xff, 0xd8})
	q := make([]byte, 65)
	for i := 1; i < len(q); i++ {
		q[i] = 1
	}
	jpegSegment(b, 0xdb, q)
	marker := byte(0xc0)
	if progressive {
		marker = 0xc2
	}
	jpegSegment(b, marker, []byte{8, byte(h >> 8), byte(h), byte(w >> 8), byte(w), 1, 1, 0x11, 0})
	ht := make([]byte, 36)
	ht[1] = 1
	ht[18] = 0x10
	ht[19] = 1 // one-bit code 0 -> DC category zero / EOB
	jpegSegment(b, 0xc4, ht)
}
func constantJPEG(w, h, restart int) []byte {
	var b bytes.Buffer
	jpegConstantHeader(&b, w, h, false)
	if restart > 0 {
		jpegSegment(&b, 0xdd, []byte{byte(restart >> 8), byte(restart)})
	}
	jpegSegment(&b, 0xda, []byte{1, 1, 0, 0, 63, 0})
	blocks := ((w + 7) / 8) * ((h + 7) / 8)
	bits := 0
	rst := byte(0xd0)
	flush := func() {
		if bits > 0 {
			b.WriteByte(byte((1 << uint(8-bits)) - 1))
			bits = 0
		}
	}
	for i := 0; i < blocks; i++ {
		bits += 2
		if bits == 8 {
			b.WriteByte(0)
			bits = 0
		}
		if restart > 0 && (i+1)%restart == 0 && i+1 < blocks {
			flush()
			b.Write([]byte{0xff, rst})
			rst = 0xd0 + (rst-0xd0+1)%8
		}
	}
	flush()
	b.Write([]byte{0xff, 0xd9})
	return b.Bytes()
}
func compareJPEG(t *testing.T, data []byte, mw, mh int) {
	t.Helper()
	ref, err := jpeg.Decode(bytes.NewReader(data))
	if err != nil {
		t.Fatal("reference", err)
	}
	out, err := Decode(context.Background(), bytes.NewReader(data), mw, mh)
	if err != nil {
		t.Fatal(err)
	}
	ow, oh := fit(ref.Bounds().Dx(), ref.Bounds().Dy(), mw, mh)
	if out.Bounds() != image.Rect(0, 0, ow, oh) {
		t.Fatal(out.Bounds())
	}
	maxError := 0
	for y := 0; y < oh; y++ {
		for x := 0; x < ow; x++ {
			c := color.GrayModel.Convert(ref.At(sample(x, ref.Bounds().Dx(), ow), sample(y, ref.Bounds().Dy(), oh))).(color.Gray)
			diff := abs(int(out.GrayAt(x, y).Y) - int(c.Y))
			if diff > maxError {
				maxError = diff
			}
			if diff > 2 {
				t.Fatalf("(%d,%d): got %d reference %d (error %d)", x, y, out.GrayAt(x, y).Y, c.Y, diff)
			}
		}
	}
	t.Logf("maximum grayscale error versus standard JPEG: %d", maxError)
}
func TestJPEGGrayColorAndDownsample(t *testing.T) {
	for _, gray := range []bool{false, true} {
		for _, wh := range [][2]int{{1, 1}, {7, 9}, {17, 31}, {101, 67}} {
			var src image.Image
			if gray {
				m := image.NewGray(image.Rect(0, 0, wh[0], wh[1]))
				for i := range m.Pix {
					m.Pix[i] = byte(i * 13)
				}
				src = m
			} else {
				m := image.NewNRGBA(image.Rect(0, 0, wh[0], wh[1]))
				for y := 0; y < wh[1]; y++ {
					for x := 0; x < wh[0]; x++ {
						m.SetNRGBA(x, y, color.NRGBA{byte(x*17 + y*5), byte(x*3 + y*23), byte(x*31 + y*13), 255})
					}
				}
				src = m
			}
			var b bytes.Buffer
			if err := jpeg.Encode(&b, src, &jpeg.Options{Quality: 91}); err != nil {
				t.Fatal(err)
			}
			t.Run(fmt.Sprintf("gray%v-%dx%d", gray, wh[0], wh[1]), func(t *testing.T) { compareJPEG(t, b.Bytes(), 128, 128); compareJPEG(t, b.Bytes(), 7, 5) })
		}
	}
}
func TestJPEGRestartMarkers(t *testing.T) {
	for _, restart := range []int{0, 1, 3, 8, 17} {
		t.Run(fmt.Sprint(restart), func(t *testing.T) { compareJPEG(t, constantJPEG(123, 71, restart), 37, 31) })
	}
	data := constantJPEG(24, 16, 1)
	i := bytes.Index(data, []byte{0xff, 0xd0})
	if i < 0 {
		t.Fatal("no RST")
	}
	data[i+1] = 0xd2
	if _, err := Decode(context.Background(), bytes.NewReader(data), 10, 10); err == nil {
		t.Fatal("accepted bad RST")
	}
}
func TestJPEGLargeBoundedMemory(t *testing.T) {
	if testing.Short() {
		t.Skip("96-million-pixel streaming test")
	}
	data := constantJPEG(12000, 8000, 0)
	assertSmallDecodeAllocation(t, data, 2<<20)
	// Also cover three-component 4:2:0 entropy and chroma with a source that
	// would require approximately 144 MB even in full-frame planar form.
	assertSmallDecodeAllocation(t, sampledJPEG(12000, 8000, 2, 2, false, [3]int{0, 1, 2}), 2<<20)
	ctx, cancel := context.WithCancel(context.Background())
	r := &cancelReader{Reader: bytes.NewReader(data), cancel: cancel, remaining: 8192}
	if _, err := Decode(ctx, r, 128, 128); !errors.Is(err, context.Canceled) {
		t.Fatal(err)
	}
}
func TestJPEGProgressiveFallbackAndLimits(t *testing.T) {
	var b bytes.Buffer
	jpegConstantHeader(&b, 8, 8, true)
	jpegSegment(&b, 0xda, []byte{1, 1, 0, 0, 0, 0})
	b.WriteByte(0x7f)
	jpegSegment(&b, 0xda, []byte{1, 1, 0, 1, 63, 0})
	b.WriteByte(0x7f)
	b.Write([]byte{0xff, 0xd9})
	compareJPEG(t, b.Bytes(), 8, 8)
	for _, tc := range []struct {
		w, h        int
		progressive bool
		want        error
	}{{1024, 1024, true, ErrUnsupported}, {1, 65535, true, ErrUnsupported}, {65535, 65535, false, ErrLimit}, {0, 10, false, ErrLimit}} {
		b.Reset()
		jpegConstantHeader(&b, tc.w, tc.h, tc.progressive)
		jpegSegment(&b, 0xda, []byte{1, 1, 0, 0, 63, 0})
		if _, err := Decode(context.Background(), bytes.NewReader(b.Bytes()), 10, 10); !errors.Is(err, tc.want) {
			t.Fatalf("%+v: %v", tc, err)
		}
	}
}
func TestJPEGTruncatedAndCorrupt(t *testing.T) {
	data := constantJPEG(31, 19, 3)
	for n := 0; n < len(data); n++ {
		if _, err := Decode(context.Background(), bytes.NewReader(data[:n]), 10, 10); err == nil {
			t.Fatalf("accepted truncation at %d", n)
		}
	}
	// Malformed Huffman trees must not index outside tables.
	for _, counts := range [][]byte{{3}, {2}, {0, 5}, {0, 0, 9}} {
		bad := append([]byte(nil), data...)
		i := bytes.Index(bad, []byte{0xff, 0xc4})
		if i < 0 {
			t.Fatal("DHT")
		}
		copy(bad[i+5:i+21], counts)
		if _, err := Decode(context.Background(), bytes.NewReader(bad), 10, 10); err == nil {
			t.Fatal("accepted oversubscribed DHT")
		}
	}
	// Full marker payload length is checked before allocation and access.
	for _, bad := range [][]byte{{0xff, 0xd8, 0xff, 0xdb, 0, 1}, {0xff, 0xd8, 0xff, 0xc0, 0, 2}, {0xff, 0xd8, 0xff, 0xda, 0, 2}} {
		if _, err := Decode(context.Background(), bytes.NewReader(bad), 10, 10); err == nil {
			t.Fatal("accepted bad segment")
		}
	}
}
