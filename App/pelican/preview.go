package main

import (
	"errors"
	"image"
	"image/color"
	"image/gif"
	"os"
	"time"
)

func writePreview(path string, interval time.Duration) error {
	const scale = 3
	g := gif.GIF{LoopCount: 0}
	palette := color.Palette{color.White, color.Black}
	for step := 0; step < frameCount; step++ {
		source := render(step)
		dst := image.NewPaletted(image.Rect(0, 0, source.Bounds().Dx()*scale, source.Bounds().Dy()*scale), palette)
		for y := 0; y < dst.Bounds().Dy(); y++ {
			for x := 0; x < dst.Bounds().Dx(); x++ {
				if source.GrayAt(x/scale, y/scale).Y < 128 {
					dst.SetColorIndex(x, y, 1)
				}
			}
		}
		g.Image = append(g.Image, dst)
		g.Delay = append(g.Delay, int(interval/(10*time.Millisecond)))
		g.Disposal = append(g.Disposal, gif.DisposalNone)
	}
	file, err := os.Create(path)
	if err != nil {
		return err
	}
	return errors.Join(gif.EncodeAll(file, &g), file.Close())
}
