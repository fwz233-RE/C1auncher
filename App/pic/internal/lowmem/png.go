package lowmem

import (
	"bufio"
	"compress/zlib"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"hash/crc32"
	"image"
	"io"
)

var pngSignature = []byte{137, 80, 78, 71, 13, 10, 26, 10}

// pngChunks holds only a chunk header and rolling CRC, never chunk-sized data.
type pngChunks struct {
	r      *bufio.Reader
	kind   string
	left   uint32
	crc    uint32
	count  int
	active bool
}

func (p *pngChunks) next() error {
	if p.active {
		return errors.New("lowmem: unfinished PNG chunk")
	}
	var b [8]byte
	if _, err := io.ReadFull(p.r, b[:]); err != nil {
		return err
	}
	p.count++
	if p.count > 65536 {
		return fmt.Errorf("%w: too many PNG chunks", ErrLimit)
	}
	p.left = binary.BigEndian.Uint32(b[:4])
	if int64(p.left) > MaxFileBytes {
		return fmt.Errorf("%w: PNG chunk length", ErrLimit)
	}
	for _, c := range b[4:] {
		if !(c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z') {
			return errors.New("lowmem: invalid PNG chunk type")
		}
	}
	if b[6]&32 != 0 {
		return errors.New("lowmem: invalid PNG reserved chunk bit")
	}
	p.kind = string(b[4:])
	p.crc = crc32.Update(0, crc32.IEEETable, b[4:])
	p.active = true
	return nil
}
func (p *pngChunks) Read(b []byte) (int, error) {
	if len(b) == 0 {
		return 0, nil
	}
	if p.left == 0 {
		return 0, io.EOF
	}
	if uint64(len(b)) > uint64(p.left) {
		b = b[:int(p.left)]
	}
	n, err := p.r.Read(b)
	p.left -= uint32(n)
	p.crc = crc32.Update(p.crc, crc32.IEEETable, b[:n])
	return n, err
}
func (p *pngChunks) finish() error {
	if !p.active || p.left != 0 {
		return errors.New("lowmem: incomplete PNG chunk")
	}
	var b [4]byte
	if _, err := io.ReadFull(p.r, b[:]); err != nil {
		return err
	}
	if binary.BigEndian.Uint32(b[:]) != p.crc {
		return errors.New("lowmem: PNG CRC mismatch in " + p.kind)
	}
	p.active = false
	return nil
}
func (p *pngChunks) payload(b []byte) error {
	if uint64(len(b)) != uint64(p.left) {
		return errors.New("lowmem: wrong PNG " + p.kind + " length")
	}
	if _, err := io.ReadFull(p, b); err != nil {
		return err
	}
	return p.finish()
}
func (p *pngChunks) skip() error {
	var scratch [4096]byte
	if _, err := io.CopyBuffer(io.Discard, p, scratch[:]); err != nil {
		return err
	}
	return p.finish()
}

// IDAT is one zlib stream across consecutive chunks. Implementing ReadByte
// prevents zlib from reading past its trailer into subsequent PNG chunks.
type pngIDAT struct {
	p    *pngChunks
	done bool
}

func (r *pngIDAT) Read(b []byte) (int, error) {
	if len(b) == 0 {
		return 0, nil
	}
	for !r.done {
		if r.p.left > 0 {
			return r.p.Read(b)
		}
		if err := r.p.finish(); err != nil {
			return 0, err
		}
		if err := r.p.next(); err != nil {
			return 0, err
		}
		if r.p.kind != "IDAT" {
			r.done = true
		}
	}
	return 0, io.EOF
}
func (r *pngIDAT) ReadByte() (byte, error) {
	var b [1]byte
	_, err := io.ReadFull(r, b[:])
	return b[0], err
}
func (r *pngIDAT) finish() error {
	var b [1]byte
	n, err := r.Read(b[:])
	if n != 0 {
		return errors.New("lowmem: extra compressed PNG data")
	}
	if err != io.EOF {
		return err
	}
	return nil
}

type pngInfo struct {
	w, h, depth, kind, channels int
	palette                     [256][4]uint32
	paletteLen                  int
	transparent                 bool
	tr                          [3]uint16
}

func decodePNG(ctx context.Context, r *bufio.Reader, mw, mh int) (*image.Gray, error) {
	var sig [8]byte
	if _, err := io.ReadFull(r, sig[:]); err != nil {
		return nil, err
	}
	if string(sig[:]) != string(pngSignature) {
		return nil, errors.New("lowmem: invalid PNG signature")
	}
	p := pngChunks{r: r}
	if err := p.next(); err != nil {
		return nil, err
	}
	if p.kind != "IHDR" {
		return nil, errors.New("lowmem: PNG missing IHDR")
	}
	var hdr [13]byte
	if err := p.payload(hdr[:]); err != nil {
		return nil, err
	}
	w, h := binary.BigEndian.Uint32(hdr[:4]), binary.BigEndian.Uint32(hdr[4:8])
	// Check before converting uint32 to int on mipsle.
	if w > MaxDimension || h > MaxDimension {
		return nil, fmt.Errorf("%w: PNG dimensions", ErrLimit)
	}
	info := pngInfo{w: int(w), h: int(h), depth: int(hdr[8]), kind: int(hdr[9])}
	if err := sourceLimits(info.w, info.h); err != nil {
		return nil, err
	}
	switch info.kind {
	case 0:
		info.channels = 1
		if info.depth != 1 && info.depth != 2 && info.depth != 4 && info.depth != 8 && info.depth != 16 {
			return nil, errors.New("lowmem: invalid grayscale PNG depth")
		}
	case 2:
		info.channels = 3
	case 3:
		info.channels = 1
		if info.depth != 1 && info.depth != 2 && info.depth != 4 && info.depth != 8 {
			return nil, errors.New("lowmem: invalid palette PNG depth")
		}
	case 4:
		info.channels = 2
	case 6:
		info.channels = 4
	default:
		return nil, errors.New("lowmem: invalid PNG color type")
	}
	if info.kind != 0 && info.kind != 3 && info.depth != 8 && info.depth != 16 {
		return nil, errors.New("lowmem: invalid PNG bit depth")
	}
	if hdr[10] != 0 || hdr[11] != 0 || hdr[12] > 1 {
		return nil, errors.New("lowmem: invalid PNG compression, filter, or interlace method")
	}
	if hdr[12] == 1 {
		return nil, fmt.Errorf("%w: Adam7 interlaced PNG", ErrUnsupported)
	}
	rowBytes64 := (int64(info.w)*int64(info.channels*info.depth) + 7) / 8
	if (rowBytes64+1)*int64(info.h) > MaxDecodedBytes {
		return nil, fmt.Errorf("%w: decompressed PNG scanlines", ErrLimit)
	}
	rowBytes := int(rowBytes64)
	bpp := max(1, (info.channels*info.depth+7)/8)
	seenPLTE, seenTRNS := false, false
	for {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		if err := p.next(); err != nil {
			return nil, err
		}
		switch p.kind {
		case "PLTE":
			if seenPLTE || seenTRNS || info.kind == 0 || info.kind == 4 || p.left == 0 || p.left > 768 || p.left%3 != 0 {
				return nil, errors.New("lowmem: invalid PNG palette")
			}
			info.paletteLen = int(p.left) / 3
			if info.kind == 3 && info.paletteLen > 1<<info.depth {
				return nil, errors.New("lowmem: PNG palette exceeds bit depth")
			}
			var data [768]byte
			if err := p.payload(data[:int(p.left)]); err != nil {
				return nil, err
			}
			for i := 0; i < info.paletteLen; i++ {
				info.palette[i] = [4]uint32{uint32(data[3*i]) * 257, uint32(data[3*i+1]) * 257, uint32(data[3*i+2]) * 257, 65535}
			}
			seenPLTE = true
		case "tRNS":
			if seenTRNS {
				return nil, errors.New("lowmem: duplicate PNG transparency")
			}
			var data [256]byte
			n := int(p.left)
			switch info.kind {
			case 0:
				if n != 2 {
					return nil, errors.New("lowmem: invalid PNG grayscale transparency")
				}
			case 2:
				if n != 6 {
					return nil, errors.New("lowmem: invalid PNG RGB transparency")
				}
			case 3:
				if !seenPLTE || n < 1 || n > info.paletteLen {
					return nil, errors.New("lowmem: invalid PNG palette transparency")
				}
			default:
				return nil, errors.New("lowmem: PNG transparency with alpha channel")
			}
			if err := p.payload(data[:n]); err != nil {
				return nil, err
			}
			if info.kind == 3 {
				for i := 0; i < n; i++ {
					info.palette[i][3] = uint32(data[i]) * 257
				}
			} else {
				info.transparent = true
				for i := 0; i < n/2; i++ {
					info.tr[i] = binary.BigEndian.Uint16(data[2*i:])
					if uint32(info.tr[i]) > uint32(1<<info.depth)-1 {
						return nil, errors.New("lowmem: PNG transparency sample out of range")
					}
				}
			}
			seenTRNS = true
		case "IDAT":
			if info.kind == 3 && !seenPLTE {
				return nil, errors.New("lowmem: PNG missing palette")
			}
			goto pixels
		default:
			if p.kind[0]&32 == 0 {
				return nil, errors.New("lowmem: unexpected critical PNG chunk " + p.kind)
			}
			if err := p.skip(); err != nil {
				return nil, err
			}
		}
	}
pixels:
	idat := pngIDAT{p: &p}
	zr, err := zlib.NewReader(&idat)
	if err != nil {
		return nil, err
	}
	defer zr.Close()
	ow, oh := fit(info.w, info.h, mw, mh)
	out := image.NewGray(image.Rect(0, 0, ow, oh))
	// Exactly two scanlines are retained, regardless of source height.
	prev, row := make([]byte, rowBytes), make([]byte, rowBytes)
	oy := 0
	for y := 0; y < info.h; y++ {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		var filter [1]byte
		if _, err := io.ReadFull(zr, filter[:]); err != nil {
			return nil, err
		}
		if _, err := io.ReadFull(zr, row); err != nil {
			return nil, err
		}
		if err := unfilter(row, prev, bpp, filter[0]); err != nil {
			return nil, err
		}
		// Validate all palette indices, even rows/pixels not selected for output.
		if info.kind == 3 {
			for x := 0; x < info.w; x++ {
				if int(pngSample(row, x, info.depth)) >= info.paletteLen {
					return nil, errors.New("lowmem: PNG palette index out of range")
				}
			}
		}
		if oy < oh && sample(oy, info.h, oh) == y {
			for x := 0; x < ow; x++ {
				out.Pix[oy*out.Stride+x] = info.gray(row, sample(x, info.w, ow))
			}
			oy++
		}
		prev, row = row, prev
	}
	var extra [1]byte
	n, err := zr.Read(extra[:])
	if n != 0 {
		return nil, fmt.Errorf("%w: PNG contains excess decompressed data", ErrLimit)
	}
	if err != io.EOF {
		if err == nil {
			err = io.ErrNoProgress
		}
		return nil, err
	}
	if err := idat.finish(); err != nil {
		return nil, err
	}
	for {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		if p.kind == "IEND" {
			if p.left != 0 {
				return nil, errors.New("lowmem: invalid PNG IEND length")
			}
			if err := p.finish(); err != nil {
				return nil, err
			}
			return out, nil
		}
		if p.kind == "IDAT" || p.kind == "tRNS" || p.kind == "PLTE" || p.kind[0]&32 == 0 {
			return nil, errors.New("lowmem: invalid PNG chunk after image data: " + p.kind)
		}
		if err := p.skip(); err != nil {
			return nil, err
		}
		if err := p.next(); err != nil {
			return nil, err
		}
	}
}

func unfilter(row, prev []byte, bpp int, filter byte) error {
	if filter > 4 {
		return errors.New("lowmem: invalid PNG row filter")
	}
	if filter == 0 {
		return nil
	}
	for x := range row {
		var a, c byte
		if x >= bpp {
			a = row[x-bpp]
			c = prev[x-bpp]
		}
		b := prev[x]
		switch filter {
		case 1:
			row[x] += a
		case 2:
			row[x] += b
		case 3:
			row[x] += byte((int(a) + int(b)) / 2)
		case 4:
			row[x] += paeth(a, b, c)
		}
	}
	return nil
}
func paeth(a, b, c byte) byte {
	p := int(a) + int(b) - int(c)
	pa, pb, pc := abs(p-int(a)), abs(p-int(b)), abs(p-int(c))
	if pa <= pb && pa <= pc {
		return a
	}
	if pb <= pc {
		return b
	}
	return c
}
func abs(x int) int {
	if x < 0 {
		return -x
	}
	return x
}
func pngSample(row []byte, index, depth int) uint16 {
	switch depth {
	case 16:
		return binary.BigEndian.Uint16(row[2*index:])
	case 8:
		return uint16(row[index])
	default:
		return uint16(row[index*depth/8]>>uint(8-depth-index*depth%8)) & uint16((1<<depth)-1)
	}
}
func (p *pngInfo) gray(row []byte, x int) uint8 {
	if p.kind == 3 {
		c := p.palette[pngSample(row, x, p.depth)]
		return grayWhite(c[0], c[1], c[2], c[3])
	}
	var v [4]uint16
	for i := 0; i < p.channels; i++ {
		v[i] = pngSample(row, x*p.channels+i, p.depth)
	}
	maxSample := uint32((1 << p.depth) - 1)
	scale := func(v uint16) uint32 { return uint32(v) * 65535 / maxSample }
	a := uint32(65535)
	switch p.kind {
	case 0, 4:
		if p.kind == 4 {
			a = scale(v[1])
		} else if p.transparent && v[0] == p.tr[0] {
			a = 0
		}
		g := scale(v[0])
		return grayWhite(g, g, g, a)
	default:
		if p.kind == 6 {
			a = scale(v[3])
		} else if p.transparent && v[0] == p.tr[0] && v[1] == p.tr[1] && v[2] == p.tr[2] {
			a = 0
		}
		return grayWhite(scale(v[0]), scale(v[1]), scale(v[2]), a)
	}
}
