package main

import (
	"encoding/json"
	"image"
	"image/color"
	"math"
	"os"
	"path/filepath"
	"time"
)

type visualMode uint8

const (
	visualStatic visualMode = iota
	visualBars
	visualRecord
	visualModeCount
	staticRefreshInterval = 10 * time.Second
	motionRefreshInterval = 700 * time.Millisecond
)

func (mode visualMode) next() visualMode { return (mode + 1) % visualModeCount }

func visualModeLabel(mode visualMode) string {
	switch mode {
	case visualBars:
		return "律动 / 低频"
	case visualRecord:
		return "唱片 / 低频"
	default:
		return "静态 / 省电"
	}
}

func progressInterval(mode visualMode) time.Duration {
	if mode == visualBars || mode == visualRecord {
		return time.Second
	}
	return staticRefreshInterval
}

// Animation is decorative, NOT an FFT or an audio-level measurement. Its
// phase advances on the active-only motion clock, independently of ffplay stats.
func visualPhase(state appState) int {
	if !state.playing {
		return 0
	}
	return int(state.visualTick % 16)
}

func shouldDrawProgress(state appState, last, now time.Time) bool {
	return state.playing && !state.paused && (last.IsZero() || now.Sub(last) >= progressInterval(state.visual))
}

func loadVisualMode(path string) visualMode {
	file, err := os.Open(path)
	if err != nil {
		return visualStatic
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil || info.Size() > 256 {
		return visualStatic
	}
	var settings struct {
		Visual visualMode `json:"visual"`
	}
	if json.NewDecoder(file).Decode(&settings) != nil || settings.Visual >= visualModeCount {
		return visualStatic
	}
	return settings.Visual
}

func saveVisualMode(path string, mode visualMode) error {
	if mode >= visualModeCount {
		return os.ErrInvalid
	}
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		return err
	}
	temporary, err := os.CreateTemp(filepath.Dir(path), ".display-*.tmp")
	if err != nil {
		return err
	}
	defer os.Remove(temporary.Name())
	err = json.NewEncoder(temporary).Encode(struct {
		Visual visualMode `json:"visual"`
	}{mode})
	if err == nil {
		err = temporary.Sync()
	}
	if closeErr := temporary.Close(); err == nil {
		err = closeErr
	}
	if err != nil {
		return err
	}
	return replaceArtworkCache(temporary.Name(), path)
}

// A small monochrome turntable illustration: fine concentric grooves, a
// suspended tonearm and a sparse offset dot shadow, drawn at panel resolution.
func recordArtwork(phase int, animated bool) *image.Gray {
	art := image.NewGray(image.Rect(0, 0, defaultCoverSize, defaultCoverSize))
	fillCover(art, 255)
	// Corner registration marks give the sleeve a printed, rather than card, feel.
	artLine(art, 0, 0, 10, 0, 0)
	artLine(art, 0, 0, 0, 10, 0)
	artLine(art, 97, 87, 97, 97, 0)
	artLine(art, 87, 97, 97, 97, 0)
	for y := 8; y < 95; y += 3 {
		for x := 8; x < 95; x += 3 {
			dx, dy := x-49, y-54
			if dx*dx+dy*dy < 40*40 {
				art.SetGray(x, y, color.Gray{Y: 0})
			}
		}
	}
	fillCoverCircle(art, 45, 47, 40, 0)
	fillCoverCircle(art, 45, 47, 38, 255)
	for _, radius := range []int{35, 31, 27, 23, 19} {
		artCircle(art, 45, 47, radius, 0)
	}
	fillCoverCircle(art, 45, 47, 14, 0)
	fillCoverCircle(art, 45, 47, 3, 255)
	// A white label tick makes the slow rotation legible without moving the grooves.
	angle := -math.Pi / 3
	if animated {
		angle += float64(phase%16) * math.Pi / 8
	}
	for r := 7; r <= 11; r++ {
		x := 45 + int(math.Round(math.Cos(angle)*float64(r)))
		y := 47 + int(math.Round(math.Sin(angle)*float64(r)))
		fillCoverCircle(art, x, y, 1, 255)
	}
	// Tonearm has a white separation from the record beneath it.
	for offset := -2; offset <= 2; offset++ {
		artLine(art, 86+offset, 12, 86+offset, 49, 255)
		artLine(art, 86+offset, 49, 72+offset, 66, 255)
	}
	artLine(art, 86, 12, 86, 49, 0)
	artLine(art, 86, 49, 72, 66, 0)
	fillCoverCircle(art, 86, 12, 4, 0)
	fillCoverCircle(art, 86, 12, 2, 255)
	for offset := -1; offset <= 1; offset++ {
		artLine(art, 74+offset, 62, 68+offset, 68, 0)
	}
	artLine(art, 6, 94, 35, 94, 0)
	artLine(art, 6, 91, 17, 91, 0)
	return art
}

func barsArtwork(phase int, active bool) *image.Gray {
	art := image.NewGray(image.Rect(0, 0, defaultCoverSize, defaultCoverSize))
	fillCover(art, 255)
	// Open graph with discrete segments, not a solid dark spectrum panel.
	for _, y := range []int{18, 38, 58, 78} {
		for x := 5; x < 94; x += 4 {
			art.SetGray(x, y, color.Gray{Y: 0})
		}
	}
	for column := 0; column < 12; column++ {
		levels := 1
		if active {
			// Deterministic display motion; deliberately independent of audio samples.
			levels = 2 + (column*7+phase*5+(column+phase)*(column+phase))%12
		}
		x := 9 + column*7
		for segment := 0; segment < levels; segment++ {
			y := 77 - segment*5
			for row := 0; row < 3; row++ {
				artLine(art, x, y-row, x+3, y-row, 0)
			}
		}
		artLine(art, x, 85, x+3, 85, 0)
	}
	artLine(art, 5, 92, 92, 92, 0)
	return art
}

func artCircle(art *image.Gray, cx, cy, radius int, value uint8) {
	x, y, err := radius, 0, 1-radius
	for x >= y {
		for _, p := range [8]image.Point{{x, y}, {y, x}, {-y, x}, {-x, y}, {-x, -y}, {-y, -x}, {y, -x}, {x, -y}} {
			art.SetGray(cx+p.X, cy+p.Y, color.Gray{Y: value})
		}
		y++
		if err < 0 {
			err += 2*y + 1
		} else {
			x--
			err += 2*(y-x) + 1
		}
	}
}

func artLine(art *image.Gray, x0, y0, x1, y1 int, value uint8) {
	dx, dy := int(math.Abs(float64(x1-x0))), -int(math.Abs(float64(y1-y0)))
	sx, sy := -1, -1
	if x0 < x1 {
		sx = 1
	}
	if y0 < y1 {
		sy = 1
	}
	err := dx + dy
	for {
		art.SetGray(x0, y0, color.Gray{Y: value})
		if x0 == x1 && y0 == y1 {
			return
		}
		twice := 2 * err
		if twice >= dy {
			err += dy
			x0 += sx
		}
		if twice <= dx {
			err += dx
			y0 += sy
		}
	}
}
