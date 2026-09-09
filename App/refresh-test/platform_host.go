//go:build !linux || !mipsle

package main

import (
	"errors"
	"image"
	"image/png"
	"os"
	"path/filepath"
)

func openDisplay() (display, error) {
	return nil, errors.New("interactive display requires C1-Slim Linux/MIPS; use --preview on this computer")
}
func writePreview(output string, f frame) error {
	img := image.NewGray(image.Rect(0, 0, width*3, height*3))
	for y := 0; y < height*3; y++ {
		for x := 0; x < width*3; x++ {
			v := byte(255)
			if f[(y/3/8)*width+x/3]&(0x80>>uint(y/3%8)) != 0 {
				v = 0
			}
			img.Pix[y*img.Stride+x] = v
		}
	}
	out, err := os.CreateTemp(filepath.Dir(output), ".refresh-preview-*.png")
	if err != nil {
		return err
	}
	defer os.Remove(out.Name())
	err = png.Encode(out, img)
	err = errors.Join(err, out.Close())
	if err != nil {
		return err
	}
	return os.Rename(out.Name(), output)
}
