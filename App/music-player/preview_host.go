//go:build !linux || !mipsle

package main

import (
	"fmt"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"time"

	"c1device"
)

// Host-only previews use precisely the same renderer and packed one-bit frame
// as the physical panel, not a separate HTML approximation of the design.
func handlePreview(args []string) (bool, error) {
	if len(args) == 0 || args[0] != "--preview" {
		return false, nil
	}
	if len(args) != 2 {
		return true, fmt.Errorf("usage: music-player --preview OUTPUT_DIRECTORY (set C1_FONT_PATH)")
	}
	data, err := os.ReadFile(environmentOrDefault("C1_FONT_PATH", defaultFontPath))
	if err != nil {
		return true, err
	}
	typeface, err := c1device.ParseTypeface(data)
	if err != nil {
		return true, err
	}
	face, err := typeface.NewFace(musicTitleFontSize)
	if err != nil {
		return true, err
	}
	defer face.Close()
	small, err := typeface.NewFace(musicLabelFontSize)
	if err != nil {
		return true, err
	}
	defer small.Close()
	if err := os.MkdirAll(args[1], 0755); err != nil {
		return true, err
	}
	state := appState{
		tracks:  []Track{{Path: "summer.mp3", Title: "夏日的风"}, {Path: "night.flac", Title: "在每一个安静的夜晚想起你"}, {Path: "03.mp3", Title: "Across the quiet sea"}},
		current: 0, selected: 0, playing: true, position: 83 * time.Second, duration: 237 * time.Second, volume: 50,
	}
	samples := []struct {
		name  string
		state appState
	}{{"01-static", state}}
	state.visual = visualBars
	samples = append(samples, struct {
		name  string
		state appState
	}{"02-bars", state})
	state.position += motionRefreshInterval
	state.visualTick++
	samples = append(samples, struct {
		name  string
		state appState
	}{"03-bars-next", state})
	state.visual = visualRecord
	samples = append(samples, struct {
		name  string
		state appState
	}{"04-record", state})
	state.visual, state.current, state.selected, state.paused = visualStatic, 1, 1, true
	state.shuffle = true
	samples = append(samples, struct {
		name  string
		state appState
	}{"05-paused-long-title", state})
	state.current, state.selected, state.paused, state.shuffle = 0, 2, false, false
	samples = append(samples, struct {
		name  string
		state appState
	}{"06-selection", state})
	state.cover = previewAlbum()
	state.selected = 0
	samples = append(samples, struct {
		name  string
		state appState
	}{"07-album", state})
	state = appState{current: -1, volume: 50}
	samples = append(samples, struct {
		name  string
		state appState
	}{"08-empty", state})
	for _, sample := range samples {
		frame := renderMusic(sample.state, face, small)
		for _, scale := range []int{1, 3} {
			name := sample.name + ".png"
			if scale > 1 {
				name = sample.name + "@3x.png"
			}
			output, err := os.Create(filepath.Join(args[1], name))
			if err != nil {
				return true, err
			}
			err = png.Encode(output, previewFrame(frame, scale))
			closeErr := output.Close()
			if err != nil {
				return true, err
			}
			if closeErr != nil {
				return true, closeErr
			}
		}
	}
	fmt.Printf("Rendered %d one-bit panel previews in %s (title line height=%d, small=%d)\n", len(samples), args[1], face.LineHeight(), small.LineHeight())
	return true, nil
}

func previewFrame(frame c1device.Frame, scale int) *image.Gray {
	out := image.NewGray(image.Rect(0, 0, c1device.DisplayWidth*scale, c1device.DisplayHeight*scale))
	for y := 0; y < out.Bounds().Dy(); y++ {
		for x := 0; x < out.Bounds().Dx(); x++ {
			px, py := x/scale, y/scale
			value := uint8(255)
			if frame[(py/8)*c1device.DisplayWidth+px]&(0x80>>(py&7)) != 0 {
				value = 0
			}
			out.SetGray(x, y, color.Gray{Y: value})
		}
	}
	return out
}

func previewAlbum() image.Image {
	art := image.NewGray(image.Rect(0, 0, 98, 98))
	fillCover(art, 255)
	fillCoverCircle(art, 65, 29, 16, 0)
	for x := 0; x < 98; x++ {
		for y := 62 + x/5; y < 98; y++ {
			art.SetGray(x, y, color.Gray{Y: uint8(40 + (x+y)%3*60)})
		}
	}
	for y := 66; y < 98; y += 5 {
		artLine(art, 0, y, 97, y-15, 255)
	}
	return art
}
