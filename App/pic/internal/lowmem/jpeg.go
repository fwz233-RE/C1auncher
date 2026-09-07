// Copyright 2009 The Go Authors. All rights reserved.
// Use of the zig-zag ordering derived from image/jpeg is governed by a
// BSD-style license that can be found in the LICENSE file.
// The bounded MCU decoder below is an independent implementation of T.81.
package lowmem

import (
	"bufio"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"image"
	"image/color"
	"io"
)

var jpegZig = [64]int{
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
}

type jpegHuffman struct {
	valid  bool
	count  [16]int
	first  [16]int
	index  [16]int
	values [256]byte
}
type jpegComponent struct {
	id              byte
	h, v, q, dc, ac int
	predictor       int32
}
type jpegStream struct {
	ctx               context.Context
	r                 *bufio.Reader
	w, h, n           int
	comp              [3]jpegComponent
	quant             [4][64]int32
	quantOK           [4]bool
	huff              [2][4]jpegHuffman
	restart           int
	bits              uint32
	nbits             uint
	jfif, adobe, rgb  bool
	transform         byte
	sawFrame, sawScan bool
}

func jpegBad(s string) error { return errors.New("lowmem: invalid JPEG: " + s) }
func (d *jpegStream) marker() (byte, error) {
	b, err := d.r.ReadByte()
	if err != nil {
		return 0, err
	}
	if b != 0xff {
		return 0, jpegBad("expected marker")
	}
	for i := 0; i < 65536; i++ {
		b, err = d.r.ReadByte()
		if err != nil {
			return 0, err
		}
		if b == 0xff {
			continue
		}
		if b == 0 {
			return 0, jpegBad("stuffed byte outside scan")
		}
		return b, nil
	}
	return 0, fmt.Errorf("%w: JPEG marker padding", ErrLimit)
}
func decodeJPEG(ctx context.Context, r *bufio.Reader, mw, mh int) (*image.Gray, error) {
	d := jpegStream{ctx: ctx, r: r}
	var soi [2]byte
	if _, err := io.ReadFull(r, soi[:]); err != nil {
		return nil, err
	}
	if soi != [2]byte{0xff, 0xd8} {
		return nil, jpegBad("missing SOI")
	}
	// Marker payloads are at most 65533 bytes, not proportional to the frame.
	var segment [65533]byte
	var out *image.Gray
	for markers := 0; markers < 65536; markers++ {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		marker, err := d.marker()
		if err != nil {
			return nil, err
		}
		if marker == 0xd9 {
			if !d.sawScan {
				return nil, jpegBad("missing scan")
			}
			return out, nil
		}
		if marker == 0xd8 || marker >= 0xd0 && marker <= 0xd7 {
			return nil, jpegBad("unexpected standalone marker")
		}
		var size [2]byte
		if _, err := io.ReadFull(r, size[:]); err != nil {
			return nil, err
		}
		n := int(binary.BigEndian.Uint16(size[:])) - 2
		if n < 0 {
			return nil, jpegBad("short segment")
		}
		p := segment[:n]
		if _, err := io.ReadFull(r, p); err != nil {
			return nil, err
		}
		switch marker {
		case 0xc0, 0xc1, 0xc2:
			if err := d.frame(p); err != nil {
				return nil, err
			}
			if marker != 0xc0 {
				return nil, fmt.Errorf("%w: progressive or extended JPEG", ErrUnsupported)
			}
		case 0xdb:
			if err := d.tables(p); err != nil {
				return nil, err
			}
		case 0xc4:
			if err := d.huffmanTables(p); err != nil {
				return nil, err
			}
		case 0xdd:
			if n != 2 {
				return nil, jpegBad("restart interval length")
			}
			d.restart = int(binary.BigEndian.Uint16(p))
		case 0xe0:
			if n >= 5 && string(p[:5]) == "JFIF\x00" {
				d.jfif = true
			}
		case 0xee:
			if n >= 12 && string(p[:5]) == "Adobe" {
				d.adobe = true
				d.transform = p[11]
			}
		case 0xda:
			if d.sawScan {
				return nil, fmt.Errorf("%w: multi-scan JPEG", ErrUnsupported)
			}
			if !d.sawFrame {
				return nil, jpegBad("scan before frame")
			}
			if d.n == 3 {
				if d.adobe && d.transform > 1 {
					return nil, fmt.Errorf("%w: JPEG color transform", ErrUnsupported)
				}
				d.rgb = !d.jfif && (d.adobe && d.transform == 0 || d.comp[0].id == 'R' && d.comp[1].id == 'G' && d.comp[2].id == 'B')
			}
			out, err = d.scan(p, mw, mh)
			if err != nil {
				return nil, err
			}
			d.sawScan = true
		default:
			if !(marker >= 0xe0 && marker <= 0xef || marker == 0xfe) {
				return nil, fmt.Errorf("%w: JPEG marker 0x%02x", ErrUnsupported, marker)
			}
		}
	}
	return nil, fmt.Errorf("%w: too many JPEG markers", ErrLimit)
}
func (d *jpegStream) frame(p []byte) error {
	if d.sawFrame || len(p) < 6 {
		return jpegBad("frame header")
	}
	d.h = int(binary.BigEndian.Uint16(p[1:3]))
	d.w = int(binary.BigEndian.Uint16(p[3:5]))
	if err := sourceLimits(d.w, d.h); err != nil {
		return err
	}
	if p[0] != 8 {
		return fmt.Errorf("%w: JPEG precision other than 8 bits", ErrUnsupported)
	}
	n := int(p[5])
	if len(p) != 6+3*n {
		return jpegBad("frame length")
	}
	if n != 1 && n != 3 {
		return fmt.Errorf("%w: JPEG component count %d", ErrUnsupported, n)
	}
	d.n = n
	total := 0
	for i := 0; i < n; i++ {
		c := &d.comp[i]
		c.id = p[6+3*i]
		c.h = int(p[7+3*i] >> 4)
		c.v = int(p[7+3*i] & 15)
		c.q = int(p[8+3*i])
		for j := 0; j < i; j++ {
			if d.comp[j].id == c.id {
				return jpegBad("duplicate component")
			}
		}
		if c.h < 1 || c.h > 4 || c.v < 1 || c.v > 4 || c.q > 3 {
			return jpegBad("sampling factors or quantization selector")
		}
		if n == 1 {
			c.h = 1
			c.v = 1
		}
		total += c.h * c.v
	}
	if total > 10 {
		return fmt.Errorf("%w: JPEG sampling factors", ErrUnsupported)
	}
	if n == 3 {
		a, b, c := d.comp[0], d.comp[1], d.comp[2]
		if a.h%b.h != 0 || a.v%b.v != 0 || b.h != c.h || b.v != c.v {
			return fmt.Errorf("%w: JPEG subsampling ratio", ErrUnsupported)
		}
	}
	d.sawFrame = true
	return nil
}
func (d *jpegStream) tables(p []byte) error {
	for len(p) > 0 {
		t := p[0]
		p = p[1:]
		if t&15 > 3 {
			return jpegBad("quantization selector")
		}
		if t>>4 != 0 {
			return fmt.Errorf("%w: 16-bit JPEG quantization", ErrUnsupported)
		}
		if len(p) < 64 {
			return jpegBad("short quantization table")
		}
		for i, v := range p[:64] {
			if v == 0 {
				return jpegBad("zero quantizer")
			}
			d.quant[t&15][jpegZig[i]] = int32(v)
		}
		d.quantOK[t&15] = true
		p = p[64:]
	}
	return nil
}
func (d *jpegStream) huffmanTables(p []byte) error {
	for len(p) > 0 {
		if len(p) < 17 {
			return jpegBad("short Huffman header")
		}
		t := p[0]
		if t>>4 > 1 || t&15 > 3 {
			return jpegBad("Huffman selector")
		}
		h := jpegHuffman{}
		code, total := 0, 0
		for i, v := range p[1:17] {
			count := int(v)
			h.count[i] = count
			h.first[i] = code
			h.index[i] = total
			if code+count >= 1<<(i+1) {
				return jpegBad("oversubscribed Huffman table or all-ones code")
			}
			total += count
			code = (code + count) << 1
		}
		if total == 0 || total > 256 || len(p) < 17+total {
			return jpegBad("Huffman table length")
		}
		copy(h.values[:], p[17:17+total])
		h.valid = true
		d.huff[t>>4][t&15] = h
		p = p[17+total:]
	}
	return nil
}
func (d *jpegStream) readBits(n uint) (uint32, error) {
	for d.nbits < n {
		b, err := d.r.ReadByte()
		if err != nil {
			return 0, err
		}
		if b == 0xff {
			next, err := d.r.ReadByte()
			if err != nil {
				return 0, err
			}
			if next != 0 {
				return 0, jpegBad("unexpected marker in entropy data")
			}
		}
		d.bits = d.bits<<8 | uint32(b)
		d.nbits += 8
	}
	d.nbits -= n
	return d.bits >> d.nbits & ((1 << n) - 1), nil
}
func (d *jpegStream) symbol(h *jpegHuffman) (byte, error) {
	code := 0
	for i := 0; i < 16; i++ {
		bit, err := d.readBits(1)
		if err != nil {
			return 0, err
		}
		code = code<<1 | int(bit)
		delta := code - h.first[i]
		if delta >= 0 && delta < h.count[i] {
			return h.values[h.index[i]+delta], nil
		}
	}
	return 0, jpegBad("invalid Huffman code")
}
func (d *jpegStream) extended(n byte) (int32, error) {
	if n == 0 {
		return 0, nil
	}
	bits, err := d.readBits(uint(n))
	if err != nil {
		return 0, err
	}
	v := int32(bits)
	if v < int32(1)<<(n-1) {
		v -= int32(1)<<n - 1
	}
	return v, nil
}
func (d *jpegStream) block(c *jpegComponent, b *[64]int32) error {
	*b = [64]int32{}
	dc, err := d.symbol(&d.huff[0][c.dc])
	if err != nil {
		return err
	}
	if dc > 11 {
		return jpegBad("baseline DC category exceeds 11")
	}
	delta, err := d.extended(dc)
	if err != nil {
		return err
	}
	c.predictor += delta
	if c.predictor < -2048 || c.predictor > 2047 {
		return jpegBad("DC coefficient out of range")
	}
	b[0] = c.predictor
	for k := 1; k < 64; {
		rs, err := d.symbol(&d.huff[1][c.ac])
		if err != nil {
			return err
		}
		run, size := int(rs>>4), rs&15
		if size == 0 {
			if run == 0 {
				break
			}
			if run != 15 {
				return jpegBad("invalid baseline AC run")
			}
			k += 16
			if k > 64 {
				return jpegBad("AC run exceeds block")
			}
			continue
		}
		if size > 10 {
			return jpegBad("baseline AC category exceeds 10")
		}
		k += run
		if k >= 64 {
			return jpegBad("AC coefficient exceeds block")
		}
		v, err := d.extended(size)
		if err != nil {
			return err
		}
		b[jpegZig[k]] = v
		k++
	}
	return nil
}
func (d *jpegStream) align() error {
	if d.nbits > 0 && d.bits&((1<<d.nbits)-1) != (1<<d.nbits)-1 {
		return jpegBad("invalid entropy padding")
	}
	d.bits = 0
	d.nbits = 0
	return nil
}
func (d *jpegStream) scan(p []byte, mw, mh int) (*image.Gray, error) {
	if len(p) < 4 || len(p) != 4+2*int(p[0]) {
		return nil, jpegBad("scan header length")
	}
	ns := int(p[0])
	if ns != d.n {
		return nil, fmt.Errorf("%w: non-interleaved multi-component JPEG scans", ErrUnsupported)
	}
	if p[len(p)-3] != 0 || p[len(p)-2] != 63 || p[len(p)-1] != 0 {
		return nil, jpegBad("baseline spectral selection")
	}
	var order [3]int
	var seen [3]bool
	for i := 0; i < ns; i++ {
		ci := -1
		for j := 0; j < d.n; j++ {
			if p[1+2*i] == d.comp[j].id {
				ci = j
				break
			}
		}
		if ci < 0 || seen[ci] {
			return nil, jpegBad("scan component selector")
		}
		seen[ci] = true
		order[i] = ci
		c := &d.comp[ci]
		c.dc = int(p[2+2*i] >> 4)
		c.ac = int(p[2+2*i] & 15)
		if c.dc > 1 || c.ac > 1 {
			return nil, jpegBad("baseline Huffman selector")
		}
		if !d.huff[0][c.dc].valid || !d.huff[1][c.ac].valid || !d.quantOK[c.q] {
			return nil, jpegBad("missing coding table")
		}
	}
	ow, oh := fit(d.w, d.h, mw, mh)
	out := image.NewGray(image.Rect(0, 0, ow, oh))
	mcuW, mcuH := 8*d.comp[0].h, 8*d.comp[0].v
	nx, ny := (d.w+mcuW-1)/mcuW, (d.h+mcuH-1)/mcuH
	// Only one MCU is materialized. Even at maximal factors this is 3 KB.
	var pixels [3][1024]byte
	var b [64]int32
	oy, mcu, rst := 0, 0, byte(0xd0)
	for my := 0; my < ny; my++ {
		yend := oy
		for yend < oh && sample(yend, d.h, oh) < (my+1)*mcuH {
			yend++
		}
		ox := 0
		for mx := 0; mx < nx; mx++ {
			if err := d.ctx.Err(); err != nil {
				return nil, err
			}
			xend := ox
			for xend < ow && sample(xend, d.w, ow) < (mx+1)*mcuW {
				xend++
			}
			wanted := yend > oy && xend > ox
			for _, ci := range order[:ns] {
				c := &d.comp[ci]
				for by := 0; by < c.v; by++ {
					for bx := 0; bx < c.h; bx++ {
						if err := d.block(c, &b); err != nil {
							return nil, err
						}
						if wanted {
							inverseBlock(&b, &d.quant[c.q], pixels[ci][:], c.h*8, bx*8, by*8)
						}
					}
				}
			}
			if wanted {
				for y := oy; y < yend; y++ {
					for x := ox; x < xend; x++ {
						sx, sy := sample(x, d.w, ow)-mx*mcuW, sample(y, d.h, oh)-my*mcuH
						var v [3]byte
						for i := 0; i < d.n; i++ {
							c := d.comp[i]
							cx, cy := sx*c.h/d.comp[0].h, sy*c.v/d.comp[0].v
							v[i] = pixels[i][cy*c.h*8+cx]
						}
						g := v[0]
						if d.n == 3 {
							rr, gg, bb := v[0], v[1], v[2]
							if !d.rgb {
								rr, gg, bb = color.YCbCrToRGB(rr, gg, bb)
							}
							g = grayWhite(uint32(rr)*257, uint32(gg)*257, uint32(bb)*257, 65535)
						}
						out.Pix[y*out.Stride+x] = g
					}
				}
			}
			ox = xend
			mcu++
			if d.restart > 0 && mcu%d.restart == 0 && mcu < nx*ny {
				if err := d.align(); err != nil {
					return nil, err
				}
				marker, err := d.marker()
				if err != nil {
					return nil, err
				}
				if marker != rst {
					return nil, jpegBad("restart marker sequence")
				}
				rst = 0xd0 + (rst-0xd0+1)%8
				for i := 0; i < d.n; i++ {
					d.comp[i].predictor = 0
				}
			}
		}
		oy = yend
	}
	if err := d.align(); err != nil {
		return nil, err
	}
	return out, nil
}

// IDCT basis: round(16384*C(u)*cos((2*x+1)*u*pi/16)), C(0)=1/sqrt(2).
// A separable fixed-point transform is used only for selected MCUs. int64
// intermediates bound even malformed, maximum-category baseline coefficients.
var idctBasis = [8][8]int64{
	{11585, 16069, 15137, 13623, 11585, 9102, 6270, 3196},
	{11585, 13623, 6270, -3196, -11585, -16069, -15137, -9102},
	{11585, 9102, -6270, -16069, -11585, 3196, 15137, 13623},
	{11585, 3196, -15137, -9102, 11585, 13623, -6270, -16069},
	{11585, -3196, -15137, 9102, 11585, -13623, -6270, 16069},
	{11585, -9102, -6270, 16069, -11585, -3196, 15137, -13623},
	{11585, -13623, 6270, 3196, -11585, 16069, -15137, 9102},
	{11585, -16069, 15137, -13623, 11585, -9102, 6270, -3196},
}

func inverseBlock(b, q *[64]int32, dst []byte, stride, bx, by int) {
	constant := true
	for _, v := range b[1:] {
		if v != 0 {
			constant = false
			break
		}
	}
	if constant {
		value := ((int64(b[0])*int64(q[0]) + 4) >> 3) + 128
		value = max(0, min(255, value))
		for y := 0; y < 8; y++ {
			for x := 0; x < 8; x++ {
				dst[(by+y)*stride+bx+x] = byte(value)
			}
		}
		return
	}
	var tmp [64]int64
	for v := 0; v < 8; v++ {
		for x := 0; x < 8; x++ {
			var sum int64
			for u := 0; u < 8; u++ {
				sum += int64(b[v*8+u]) * int64(q[v*8+u]) * idctBasis[x][u]
			}
			tmp[v*8+x] = sum
		}
	}
	for y := 0; y < 8; y++ {
		for x := 0; x < 8; x++ {
			var sum int64
			for v := 0; v < 8; v++ {
				sum += tmp[v*8+x] * idctBasis[y][v]
			}
			val := ((sum + (1 << 29)) >> 30) + 128
			if val < 0 {
				val = 0
			}
			if val > 255 {
				val = 255
			}
			dst[(by+y)*stride+bx+x] = byte(val)
		}
	}
}
