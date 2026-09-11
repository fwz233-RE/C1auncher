package c1device

import (
	"encoding/binary"
	"fmt"
	"image"
	"image/color"
	"sort"

	"golang.org/x/image/font"
	"golang.org/x/image/math/fixed"
)

// NewBitmapFace uses native 8x16/16x16 glyph pixels without scaling, hinting,
// antialiasing or coverage thresholds. Data is C1BF followed by sorted records
// of a little-endian Unicode scalar, width byte and 16 big-endian row words.
// Existing TrueType users are unaffected by this opt-in constructor.
func NewBitmapFace(data []byte, lineHeight int) (*Face, error) {
	if len(data) < 4 || string(data[:4]) != "C1BF" || (len(data)-4)%37 != 0 || len(data) == 4 || lineHeight < 16 {
		return nil, fmt.Errorf("invalid native bitmap font")
	}
	b := &bitmapFace{data: append([]byte(nil), data[4:]...), lineHeight: lineHeight, fallback: -1}
	last := rune(-1)
	for i := 0; i < len(b.data)/37; i++ {
		cp := rune(binary.LittleEndian.Uint32(b.data[i*37:]))
		w := b.data[i*37+4]
		if cp <= last || cp > 0x10ffff || cp >= 0xd800 && cp <= 0xdfff || (w != 8 && w != 16) {
			return nil, fmt.Errorf("invalid bitmap glyph %d", i)
		}
		if cp == 0xfffd {
			b.fallback = i
		}
		last = cp
	}
	if b.fallback < 0 {
		return nil, fmt.Errorf("bitmap font missing replacement glyph")
	}
	return &Face{face: b, lineHeight: lineHeight, ascent: 16}, nil
}

// Draw native row bits directly, as c1pkg_text does. This avoids per-string
// alpha masks and the TrueType rasterizer entirely for the bitmap path.
func (canvas *Canvas) drawBitmap(b *bitmapFace, x, top int, text string) {
	if top <= -16 || top >= DisplayHeight || x >= DisplayWidth {
		return
	}
	left := int64(x)
	for _, r := range text {
		i := b.index(r) * 37
		w := int(b.data[i+4])
		if left >= DisplayWidth {
			break
		}
		if left+int64(w) > 0 {
			for row := 0; row < 16; row++ {
				y := top + row
				if y < 0 || y >= DisplayHeight {
					continue
				}
				bits := binary.BigEndian.Uint16(b.data[i+5+row*2:])
				for col := 0; col < w; col++ {
					px := left + int64(col)
					if px >= 0 && px < DisplayWidth && bits&(0x8000>>uint(col)) != 0 {
						canvas.setBlack(int(px), y)
					}
				}
			}
		}
		left += int64(w)
	}
}

type bitmapFace struct {
	data       []byte
	lineHeight int
	fallback   int
}

func (b *bitmapFace) index(r rune) int {
	n := len(b.data) / 37
	i := sort.Search(n, func(i int) bool { return rune(binary.LittleEndian.Uint32(b.data[i*37:])) >= r })
	if i < n && rune(binary.LittleEndian.Uint32(b.data[i*37:])) == r {
		return i
	}
	return b.fallback
}
func (b *bitmapFace) Close() error                   { return nil }
func (b *bitmapFace) Kern(r0, r1 rune) fixed.Int26_6 { return 0 }
func (b *bitmapFace) Metrics() font.Metrics {
	return font.Metrics{Height: fixed.I(b.lineHeight), Ascent: fixed.I(16), Descent: 0, XHeight: fixed.I(8), CapHeight: fixed.I(16)}
}
func (b *bitmapFace) GlyphAdvance(r rune) (fixed.Int26_6, bool) {
	return fixed.I(int(b.data[b.index(r)*37+4])), true
}
func (b *bitmapFace) GlyphBounds(r rune) (fixed.Rectangle26_6, fixed.Int26_6, bool) {
	a, _ := b.GlyphAdvance(r)
	return fixed.Rectangle26_6{Min: fixed.P(0, -16), Max: fixed.Point26_6{X: a}}, a, true
}
func (b *bitmapFace) Glyph(dot fixed.Point26_6, r rune) (image.Rectangle, image.Image, image.Point, fixed.Int26_6, bool) {
	i := b.index(r) * 37
	w := int(b.data[i+4])
	left, top := dot.X.Floor(), dot.Y.Floor()-16
	return image.Rect(left, top, left+w, top+16), bitmapMask{rows: b.data[i+5 : i+37], width: w}, image.Point{}, fixed.I(w), true
}

// The mask reads the original bits directly, without allocating a raster.
type bitmapMask struct {
	rows  []byte
	width int
}

func (m bitmapMask) ColorModel() color.Model { return color.AlphaModel }
func (m bitmapMask) Bounds() image.Rectangle { return image.Rect(0, 0, m.width, 16) }
func (m bitmapMask) At(x, y int) color.Color {
	if x < 0 || y < 0 || x >= m.width || y >= 16 {
		return color.Alpha{}
	}
	if binary.BigEndian.Uint16(m.rows[y*2:])&(0x8000>>uint(x)) != 0 {
		return color.Alpha{A: 255}
	}
	return color.Alpha{}
}
