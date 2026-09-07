package lowmem

import (
	"bytes"
	"context"
	"fmt"
	"image/jpeg"
	"testing"
)

type jpegTestBits struct {
	b     *bytes.Buffer
	value byte
	n     uint
}

func (w *jpegTestBits) write(value uint32, n uint) {
	for n > 0 {
		n--
		w.value = w.value<<1 | byte(value>>n&1)
		w.n++
		if w.n == 8 {
			w.b.WriteByte(w.value)
			if w.value == 255 {
				w.b.WriteByte(0)
			}
			w.value = 0
			w.n = 0
		}
	}
}
func (w *jpegTestBits) flush() {
	if w.n != 0 {
		w.write((1<<(8-w.n))-1, 8-w.n)
	}
}

// This encoder deliberately changes DC values between MCU blocks. It covers
// component layout, component ordering, RGB interpretation, and byte stuffing
// without relying on a second external JPEG encoder or binary fixture files.
func sampledJPEG(width, height, h, v int, rgb bool, order [3]int) []byte {
	var b bytes.Buffer
	b.Write([]byte{0xff, 0xd8})
	if rgb {
		app := make([]byte, 12)
		copy(app, "Adobe")
		jpegSegment(&b, 0xee, app)
	}
	q := make([]byte, 65)
	for i := 1; i < len(q); i++ {
		q[i] = 1
	}
	jpegSegment(&b, 0xdb, q)
	jpegSegment(&b, 0xc0, []byte{8, byte(height >> 8), byte(height), byte(width >> 8), byte(width), 3, 1, byte(h<<4 | v), 0, 2, 0x11, 0, 3, 0x11, 0})
	ht := make([]byte, 17+12+18)
	ht[4] = 12
	for i := 0; i < 12; i++ {
		ht[17+i] = byte(i)
	}
	ht[29] = 0x10
	ht[30] = 1
	jpegSegment(&b, 0xc4, ht)
	jpegSegment(&b, 0xda, []byte{3, byte(order[0] + 1), 0, byte(order[1] + 1), 0, byte(order[2] + 1), 0, 0, 63, 0})
	bits := jpegTestBits{b: &b}
	var dc [3]int
	nx, ny := (width+8*h-1)/(8*h), (height+8*v-1)/(8*v)
	for my := 0; my < ny; my++ {
		for mx := 0; mx < nx; mx++ {
			for _, ci := range order {
				hh, vv := 1, 1
				if ci == 0 {
					hh = h
					vv = v
				}
				for by := 0; by < vv; by++ {
					for bx := 0; bx < hh; bx++ {
						next := 8 * (40 + ((mx*hh+bx)*23+(my*vv+by)*31+ci*47)%160 - 128)
						delta := next - dc[ci]
						dc[ci] = next
						category := uint(0)
						for value := abs(delta); value > 0; value >>= 1 {
							category++
						}
						bits.write(uint32(category), 4)
						amplitude := delta
						if amplitude < 0 {
							amplitude += (1 << category) - 1
						}
						bits.write(uint32(amplitude), category)
						bits.write(0, 1)
					}
				}
			}
		}
	}
	bits.flush()
	b.Write([]byte{0xff, 0xd9})
	return b.Bytes()
}
func TestJPEGSamplingLayoutsAndRGB(t *testing.T) {
	for _, hv := range [][2]int{{1, 1}, {1, 2}, {2, 1}, {2, 2}, {4, 1}, {4, 2}} {
		for _, rgb := range []bool{false, true} {
			for _, order := range [][3]int{{0, 1, 2}, {2, 0, 1}} {
				t.Run(fmt.Sprintf("%dx%d-rgb%v-order%v", hv[0], hv[1], rgb, order), func(t *testing.T) {
					data := sampledJPEG(79, 43, hv[0], hv[1], rgb, order)
					compareJPEG(t, data, 100, 100)
					compareJPEG(t, data, 11, 17)
				})
			}
		}
	}
}
func TestJPEGUncommonSmallFallback(t *testing.T) {
	// SOF1 uses the same sequential entropy syntax here but intentionally takes
	// the strictly bounded standard-library path.
	data := constantJPEG(16, 16, 0)
	i := bytes.Index(data, []byte{0xff, 0xc0})
	data[i+1] = 0xc1
	compareJPEG(t, data, 10, 10)
	// CMYK frame without scan: confirm its feature is rejected before allocating
	// any full frame; standard decoder will then report the malformed stream.
	var b bytes.Buffer
	b.Write([]byte{0xff, 0xd8})
	jpegSegment(&b, 0xc0, []byte{8, 4, 0, 4, 0, 4, 1, 0x11, 0, 2, 0x11, 0, 3, 0x11, 0, 4, 0x11, 0})
	jpegSegment(&b, 0xda, []byte{4, 1, 0, 2, 0, 3, 0, 4, 0, 0, 63, 0})
	if _, err := Decode(context.Background(), bytes.NewReader(b.Bytes()), 10, 10); err == nil {
		t.Fatal("accepted oversized CMYK")
	}
}
func TestJPEGFixtureValidForStandard(t *testing.T) {
	// Guard the independent synthetic encoder itself.
	if _, err := jpeg.Decode(bytes.NewReader(sampledJPEG(37, 29, 2, 2, false, [3]int{0, 1, 2}))); err != nil {
		t.Fatal(err)
	}
}
