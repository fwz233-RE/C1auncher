package lowmem

import (
	"bytes"
	"compress/zlib"
	"context"
	"errors"
	"testing"
)

func TestPNGChunkOrderingAndTrailingData(t *testing.T) {
	var z bytes.Buffer
	zw := zlib.NewWriter(&z)
	zw.Write([]byte{0, 42})
	zw.Close()
	type chunk struct {
		kind string
		data []byte
	}
	hdr := chunk{"IHDR", pngHeader(1, 1, 8, 0, 0)}
	idat := chunk{"IDAT", z.Bytes()}
	iend := chunk{"IEND", nil}
	for name, chunks := range map[string][]chunk{
		"duplicate header":          {hdr, hdr, idat, iend},
		"unknown critical":          {hdr, {"ABCD", nil}, idat, iend},
		"invalid reserved bit":      {hdr, {"abca", nil}, idat, iend},
		"late transparency":         {hdr, idat, {"tRNS", []byte{0, 42}}, iend},
		"late IDAT":                 {hdr, idat, {"tEXt", nil}, {"IDAT", nil}, iend},
		"nonconsecutive IDAT":       {hdr, {"IDAT", z.Bytes()[:2]}, {"tEXt", nil}, {"IDAT", z.Bytes()[2:]}, iend},
		"trailing compressed bytes": {hdr, {"IDAT", append(append([]byte(nil), z.Bytes()...), 0)}, iend},
		"invalid IEND":              {hdr, idat, {"IEND", []byte{0}}},
		"missing IDAT":              {hdr, iend},
	} {
		t.Run(name, func(t *testing.T) {
			var b bytes.Buffer
			b.Write(pngSignature)
			for _, c := range chunks {
				pngChunk(&b, c.kind, c.data)
			}
			if _, err := Decode(context.Background(), bytes.NewReader(b.Bytes()), 1, 1); err == nil {
				t.Fatal("accepted corrupt PNG")
			}
		})
	}
}
func TestPNGChunkCountLimit(t *testing.T) {
	var b bytes.Buffer
	b.Write(pngSignature)
	pngChunk(&b, "IHDR", pngHeader(1, 1, 8, 0, 0))
	for i := 0; i < 65536; i++ {
		pngChunk(&b, "tEXt", nil)
	}
	if _, err := Decode(context.Background(), bytes.NewReader(b.Bytes()), 1, 1); !errors.Is(err, ErrLimit) {
		t.Fatal(err)
	}
}
func TestPNGMaximumWidthRowMemory(t *testing.T) {
	// Maximal legal row width at eight bytes/pixel. This catches accidental
	// source-width squared allocation and 32-bit byte-count overflow.
	rows := make([]byte, (65535*8+1)*2)
	data := rawPNG(65535, 2, 16, 6, rows, nil, nil, 0)
	assertSmallDecodeAllocation(t, data, 2<<20)
}
