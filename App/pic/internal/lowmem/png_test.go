package lowmem

import (
	"bytes"
	"compress/zlib"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"hash/crc32"
	"image"
	"image/png"
	"io"
	"testing"
)

func pngChunk(b *bytes.Buffer, kind string, p []byte) {
	binary.Write(b, binary.BigEndian, uint32(len(p)))
	b.WriteString(kind)
	b.Write(p)
	crc := crc32.Update(0, crc32.IEEETable, []byte(kind))
	crc = crc32.Update(crc, crc32.IEEETable, p)
	binary.Write(b, binary.BigEndian, crc)
}
func pngHeader(w, h uint32, depth, kind, interlace byte) []byte {
	p := make([]byte, 13)
	binary.BigEndian.PutUint32(p, w)
	binary.BigEndian.PutUint32(p[4:], h)
	p[8], p[9], p[12] = depth, kind, interlace
	return p
}
func rawPNG(w, h int, depth, kind byte, rows []byte, palette, trns []byte, split int) []byte {
	var b, z bytes.Buffer
	b.Write(pngSignature)
	pngChunk(&b, "IHDR", pngHeader(uint32(w), uint32(h), depth, kind, 0))
	if palette != nil {
		pngChunk(&b, "PLTE", palette)
	}
	if trns != nil {
		pngChunk(&b, "tRNS", trns)
	}
	zw := zlib.NewWriter(&z)
	zw.Write(rows)
	zw.Close()
	data := z.Bytes()
	if split < 1 {
		split = len(data)
	}
	for len(data) > 0 {
		n := min(split, len(data))
		pngChunk(&b, "IDAT", data[:n])
		data = data[n:]
	}
	pngChunk(&b, "IEND", nil)
	return b.Bytes()
}
func comparePNG(t *testing.T, b []byte) {
	t.Helper()
	ref, err := png.Decode(bytes.NewReader(b))
	if err != nil {
		t.Fatal("reference", err)
	}
	out, err := Decode(context.Background(), bytes.NewReader(b), 4096, 1024)
	if err != nil {
		t.Fatal(err)
	}
	if out.Bounds() != ref.Bounds() {
		t.Fatal(out.Bounds(), ref.Bounds())
	}
	for y := 0; y < out.Rect.Dy(); y++ {
		for x := 0; x < out.Rect.Dx(); x++ {
			r, g, b, a := ref.At(x, y).RGBA()
			gray := ((19595*uint64(r) + 38470*uint64(g) + 7471*uint64(b) + 32768) >> 16) + 65535 - uint64(a)
			want := int((gray + 128) / 257)
			if abs(int(out.GrayAt(x, y).Y)-want) > 1 {
				t.Fatalf("(%d,%d) got %d want %d", x, y, out.GrayAt(x, y).Y, want)
			}
		}
	}
}
func TestPNGTypesDepthsFilters(t *testing.T) {
	for _, kind := range []byte{0, 2, 3, 4, 6} {
		for _, depth := range []byte{1, 2, 4, 8, 16} {
			if kind == 3 && depth == 16 || kind != 0 && kind != 3 && depth < 8 {
				continue
			}
			channels := map[byte]int{0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[kind]
			w, h := 19, 5
			rowBytes := (w*channels*int(depth) + 7) / 8
			rows := make([]byte, (rowBytes+1)*h)
			prev := make([]byte, rowBytes)
			for y := 0; y < h; y++ {
				raw := make([]byte, rowBytes)
				for x := range raw {
					raw[x] = byte(x*31 + y*57)
				}
				rows[y*(rowBytes+1)] = byte(y)
				dst := rows[y*(rowBytes+1)+1 : (y+1)*(rowBytes+1)]
				bpp := max(1, (channels*int(depth)+7)/8)
				for x, v := range raw {
					var a, c byte
					if x >= bpp {
						a = raw[x-bpp]
						c = prev[x-bpp]
					}
					b := prev[x]
					switch y {
					case 0:
						dst[x] = v
					case 1:
						dst[x] = v - a
					case 2:
						dst[x] = v - b
					case 3:
						dst[x] = v - byte((int(a)+int(b))/2)
					case 4:
						dst[x] = v - paeth(a, b, c)
					}
				}
				prev = raw
			}
			var palette, trns []byte
			if kind == 3 {
				palette = make([]byte, 3*(1<<depth))
				trns = make([]byte, 1<<depth)
				for i := range trns {
					palette[i*3], palette[i*3+1], palette[i*3+2] = byte(i), byte(255-i), byte(i*31)
					trns[i] = byte(i * 23)
				}
			}
			t.Run(fmt.Sprintf("type%d-depth%d", kind, depth), func(t *testing.T) { comparePNG(t, rawPNG(w, h, depth, kind, rows, palette, trns, 1)) })
		}
	}
}
func TestPNGTransparency(t *testing.T) {
	cases := [][]byte{
		rawPNG(2, 1, 8, 0, []byte{0, 31, 32}, nil, []byte{0, 31}, 0),
		rawPNG(2, 1, 16, 0, []byte{0, 0x12, 0x34, 0x12, 0x35}, nil, []byte{0x12, 0x34}, 0),
		rawPNG(2, 1, 8, 2, []byte{0, 10, 20, 30, 10, 20, 31}, nil, []byte{0, 10, 0, 20, 0, 30}, 0),
		rawPNG(2, 1, 16, 2, []byte{0, 0x12, 0x34, 0x23, 0x45, 0x34, 0x56, 0x12, 0x35, 0x23, 0x45, 0x34, 0x56}, nil, []byte{0x12, 0x34, 0x23, 0x45, 0x34, 0x56}, 0),
		rawPNG(2, 1, 8, 6, []byte{0, 0, 0, 0, 0, 0, 0, 0, 128}, nil, nil, 0),
	}
	for i, b := range cases {
		t.Run(fmt.Sprint(i), func(t *testing.T) {
			comparePNG(t, b)
			out, err := Decode(context.Background(), bytes.NewReader(b), 2, 1)
			if err != nil {
				t.Fatal(err)
			}
			if out.Pix[0] != 255 {
				t.Fatal(out.Pix)
			}
		})
	}
}
func TestPNGLargeBoundedMemory(t *testing.T) {
	if testing.Short() {
		t.Skip("96-million-pixel streaming test")
	}
	var b, z bytes.Buffer
	b.Write(pngSignature)
	pngChunk(&b, "IHDR", pngHeader(12000, 8000, 8, 0, 0))
	zw, _ := zlib.NewWriterLevel(&z, zlib.BestSpeed)
	row := make([]byte, 12001)
	for y := 0; y < 8000; y++ {
		if _, err := zw.Write(row); err != nil {
			t.Fatal(err)
		}
	}
	zw.Close()
	pngChunk(&b, "IDAT", z.Bytes())
	pngChunk(&b, "IEND", nil)
	assertSmallDecodeAllocation(t, b.Bytes(), 2<<20)
	ctx, cancel := context.WithCancel(context.Background())
	r := &cancelReader{Reader: bytes.NewReader(b.Bytes()), cancel: cancel, remaining: 8192}
	if _, err := Decode(ctx, r, 128, 128); !errors.Is(err, context.Canceled) {
		t.Fatal(err)
	}
}
func TestPNGCorruptionAndLimits(t *testing.T) {
	valid := rawPNG(2, 2, 8, 0, []byte{0, 1, 2, 0, 3, 4}, nil, nil, 2)
	for n := 0; n < len(valid); n++ {
		if _, err := Decode(context.Background(), bytes.NewReader(valid[:n]), 2, 2); err == nil {
			t.Fatalf("accepted truncation %d", n)
		}
	}
	for _, off := range []int{29, len(valid) - 1, 45} {
		bad := append([]byte(nil), valid...)
		bad[off] ^= 1
		if _, err := Decode(context.Background(), bytes.NewReader(bad), 2, 2); err == nil {
			t.Fatalf("accepted corrupt byte %d", off)
		}
	}
	for _, rows := range [][]byte{{0, 1}, {5, 1, 2}, {0, 1, 2, 9}} {
		b := rawPNG(2, 1, 8, 0, rows, nil, nil, 0)
		if _, err := Decode(context.Background(), bytes.NewReader(b), 2, 2); err == nil {
			t.Fatal("accepted bad scanline", rows)
		}
	}
	// An invalid palette index on an unsampled row must still fail.
	b := rawPNG(2, 2, 8, 3, []byte{0, 1, 0, 0, 0, 0}, []byte{0, 0, 0}, nil, 0)
	if _, err := Decode(context.Background(), bytes.NewReader(b), 1, 1); err == nil {
		t.Fatal("accepted invalid palette")
	}
	for _, tc := range []struct {
		w, h                   uint32
		depth, kind, interlace byte
		want                   error
	}{{65536, 1, 8, 0, 0, ErrLimit}, {0xffffffff, 2, 8, 0, 0, ErrLimit}, {20000, 20000, 8, 0, 0, ErrLimit}, {12000, 8000, 16, 6, 0, ErrLimit}, {1024, 1024, 8, 0, 1, ErrUnsupported}} {
		var b bytes.Buffer
		b.Write(pngSignature)
		pngChunk(&b, "IHDR", pngHeader(tc.w, tc.h, tc.depth, tc.kind, tc.interlace))
		if _, err := Decode(context.Background(), bytes.NewReader(b.Bytes()), 10, 10); !errors.Is(err, tc.want) {
			t.Fatalf("%+v: %v", tc, err)
		}
	}
}
func TestPNGEmptyIDATAndAncillary(t *testing.T) {
	var b, z bytes.Buffer
	b.Write(pngSignature)
	pngChunk(&b, "IHDR", pngHeader(1, 1, 8, 0, 0))
	pngChunk(&b, "tEXt", []byte("key\x00value"))
	pngChunk(&b, "IDAT", nil)
	zw := zlib.NewWriter(&z)
	zw.Write([]byte{0, 99})
	zw.Close()
	pngChunk(&b, "IDAT", z.Bytes())
	pngChunk(&b, "IDAT", nil)
	pngChunk(&b, "tEXt", []byte("after\x00data"))
	pngChunk(&b, "IEND", nil)
	comparePNG(t, b.Bytes())
	// Valid PNG chunk CRC, invalid zlib checksum must still be checked.
	z.Bytes()[z.Len()-1] ^= 1
	b.Reset()
	b.Write(pngSignature)
	pngChunk(&b, "IHDR", pngHeader(1, 1, 8, 0, 0))
	pngChunk(&b, "IDAT", z.Bytes())
	pngChunk(&b, "IEND", nil)
	if _, err := Decode(context.Background(), bytes.NewReader(b.Bytes()), 1, 1); err == nil {
		t.Fatal("accepted zlib checksum corruption")
	}
}
func TestPNGSmallAdam7Fallback(t *testing.T) {
	// At 1x1 Adam7 consists of only the first pass, so a single row is valid.
	b := rawPNG(1, 1, 8, 0, []byte{0, 73}, nil, nil, 0)
	var out bytes.Buffer
	out.Write(pngSignature)
	pngChunk(&out, "IHDR", pngHeader(1, 1, 8, 0, 1))
	out.Write(b[33:])
	comparePNG(t, out.Bytes())
}

var _ image.Image = (*image.Gray)(nil)
var _ io.Reader = (*pngIDAT)(nil)
