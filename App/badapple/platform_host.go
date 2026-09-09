//go:build !linux || !mipsle

package main

import (
	"errors"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
)

func openDisplay() (display, error) {
	return nil, errors.New("interactive playback requires C1-Slim Linux/MIPS; use --check or --preview on this computer")
}
func writePreview(path, output string) error { return writePreviewAt(path, output, 0) }
func writePreviewAt(path, output string, index uint32) error {
	a, err := openAsset(path)
	if err != nil {
		return err
	}
	defer a.close()
	f, err := a.read(index)
	if err != nil {
		return err
	}
	if st, err := os.Stat(output); err == nil && a.file != nil {
		src, err := a.file.Stat()
		if err != nil {
			return err
		}
		if os.SameFile(src, st) {
			return errors.New("preview cannot overwrite the animation")
		}
	}
	img := image.NewGray(image.Rect(0, 0, width*3, height*3))
	for y := 0; y < height*3; y++ {
		for x := 0; x < width*3; x++ {
			value := uint8(255)
			if f[(y/3/8)*width+x/3]&(0x80>>uint(y/3%8)) != 0 {
				value = 0
			}
			img.SetGray(x, y, color.Gray{Y: value})
		}
	}
	out, err := os.CreateTemp(filepath.Dir(output), ".badapple-preview-*.png")
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
