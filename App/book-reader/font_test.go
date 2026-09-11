package main

import (
	"c1device"
	"encoding/binary"
	"testing"
)

func TestReaderFontMatchesPackageManagerBits(t *testing.T) {
	f, err := newReaderFace(false)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	// Every glyph's rendering must be the original package-manager row words.
	// Reuse one screen, as the native renderer does; the comparison need not
	// allocate 1.3 GiB of transient full-screen images to check a 1 MiB font.
	c := c1device.NewCanvas()
	for i := 4; i < len(readerBitmap); i += 37 {
		r := rune(binary.LittleEndian.Uint32(readerBitmap[i:]))
		w := int(readerBitmap[i+4])
		c.Clear()
		c.DrawText(f, 0, 0, string(r))
		frame := c.Frame(128)
		for y := 0; y < 16; y++ {
			bits := binary.BigEndian.Uint16(readerBitmap[i+5+y*2:])
			for x := 0; x < w; x++ {
				want := bits&(0x8000>>uint(x)) != 0
				if (frame[(y/8)*c1device.DisplayWidth+x]&(0x80>>uint(y&7)) != 0) != want {
					t.Fatalf("U+%04X differs at %d,%d", r, x, y)
				}
			}
		}
	}
	if f.Measure("汉字ABC") != 56 {
		t.Fatal("expected native 16px Chinese, 8px ASCII")
	}
}
