package main

import (
	"c1device"
	_ "embed"
)

// Same 30064 glyphs as C1ancher's package manager, native 16px, SIL OFL 1.1.
//go:embed assets/pkg-font.bin
var readerBitmap []byte

func newReaderFace(body bool) (*c1device.Face, error) {
	height := 16
	if body {
		height = 20
	}
	return c1device.NewBitmapFace(readerBitmap, height)
}
