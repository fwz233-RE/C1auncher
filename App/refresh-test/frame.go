package main

import (
	"fmt"
	"math"
	"time"
)

const width, height = 296, 152
const frameBytes = width * height / 8

type frame [frameBytes]byte

// Original 5x7 glyphs shared with this repository's Bad Apple player.
var glyphs = map[rune][7]byte{
	'A': {14, 17, 17, 31, 17, 17, 17}, 'B': {30, 17, 17, 30, 17, 17, 30},
	'C': {14, 17, 16, 16, 16, 17, 14}, 'D': {30, 17, 17, 17, 17, 17, 30},
	'E': {31, 16, 16, 30, 16, 16, 31}, 'F': {31, 16, 16, 30, 16, 16, 16},
	'G': {14, 17, 16, 23, 17, 17, 15}, 'H': {17, 17, 17, 31, 17, 17, 17},
	'I': {14, 4, 4, 4, 4, 4, 14}, 'J': {7, 2, 2, 2, 18, 18, 12},
	'K': {17, 18, 20, 24, 20, 18, 17}, 'L': {16, 16, 16, 16, 16, 16, 31},
	'M': {17, 27, 21, 21, 17, 17, 17}, 'N': {17, 25, 21, 19, 17, 17, 17},
	'O': {14, 17, 17, 17, 17, 17, 14}, 'P': {30, 17, 17, 30, 16, 16, 16},
	'Q': {14, 17, 17, 17, 21, 18, 13}, 'R': {30, 17, 17, 30, 20, 18, 17},
	'S': {15, 16, 16, 14, 1, 1, 30}, 'T': {31, 4, 4, 4, 4, 4, 4},
	'U': {17, 17, 17, 17, 17, 17, 14}, 'V': {17, 17, 17, 17, 17, 10, 4},
	'W': {17, 17, 17, 21, 21, 21, 10}, 'X': {17, 17, 10, 4, 10, 17, 17},
	'Y': {17, 17, 10, 4, 4, 4, 4}, 'Z': {31, 1, 2, 4, 8, 16, 31},
	'0': {14, 17, 19, 21, 25, 17, 14}, '1': {4, 12, 4, 4, 4, 4, 14},
	'2': {14, 17, 1, 2, 4, 8, 31}, '3': {30, 1, 1, 14, 1, 1, 30},
	'4': {2, 6, 10, 18, 31, 2, 2}, '5': {31, 16, 16, 30, 1, 1, 30},
	'6': {14, 16, 16, 30, 17, 17, 14}, '7': {31, 1, 2, 4, 8, 8, 8},
	'8': {14, 17, 17, 14, 17, 17, 14}, '9': {14, 17, 17, 15, 1, 1, 14},
	'/': {1, 1, 2, 4, 8, 16, 16}, ':': {0, 4, 4, 0, 4, 4, 0},
	'.': {0, 0, 0, 0, 0, 4, 4}, '-': {0, 0, 0, 31, 0, 0, 0},
	'+': {0, 4, 4, 31, 4, 4, 0},
}

func pixel(f *frame, x, y int, black bool) {
	if x < 0 || x >= width || y < 0 || y >= height {
		return
	}
	off, mask := (y/8)*width+x, byte(0x80>>uint(y%8))
	if black {
		f[off] |= mask
	} else {
		f[off] &^= mask
	}
}
func rect(f *frame, x, y, w, h int, black bool) {
	for yy := y; yy < y+h; yy++ {
		for xx := x; xx < x+w; xx++ {
			pixel(f, xx, yy, black)
		}
	}
}
func text(f *frame, x, y int, s string, scale int) {
	for _, r := range s {
		for row, b := range glyphs[r] {
			for col := 0; col < 5; col++ {
				if b&(1<<uint(4-col)) != 0 {
					rect(f, x+col*scale, y+row*scale, scale, scale, true)
				}
			}
		}
		x += 6 * scale
	}
}
func circle(f *frame, cx, cy, radius int) {
	for y := -radius; y <= radius; y++ {
		for x := -radius; x <= radius; x++ {
			if x*x+y*y <= radius*radius {
				pixel(f, cx+x, cy+y, true)
			}
		}
	}
}

var modes = [...]string{"MOVE", "FLIP", "SCENE"}

func render(s state, id uint32, elapsed time.Duration, status, logLabel string) frame {
	var f frame
	text(&f, 4, 2, "REFRESH TEST / "+modes[s.mode], 1)
	text(&f, 4, 13, fmt.Sprintf("WAIT %4d MS / S%04d / %s", s.interval.Milliseconds(), s.segment, status), 1)
	text(&f, 4, 26, fmt.Sprintf("F%08d", id), 2)
	text(&f, 158, 30, fmt.Sprintf("T%06d MS", elapsed.Milliseconds()%1000000), 1)
	// 24-bit ID, most-significant bit on the left. Each pair of cells is
	// complementary: top black=1, bottom black=0. This exposes mixed frames.
	for bit := 0; bit < 24; bit++ {
		one := id&(1<<uint(23-bit)) != 0
		rect(&f, 4+bit*12, 46, 10, 8, one)
		rect(&f, 4+bit*12, 56, 10, 8, !one)
	}
	// Fixed black/white reference patches for exposure and ghosting checks.
	rect(&f, 4, 68, 12, 60, true)
	rect(&f, 280, 68, 12, 60, false)
	t := elapsed.Seconds()
	switch s.mode {
	case 0:
		phase := math.Mod(t, 4) / 2
		if phase > 1 {
			phase = 2 - phase
		}
		x := 22 + int(phase*226)
		rect(&f, x, 78, 24, 40, true)
		rect(&f, 22, 125, 250, 1, true)
	case 1:
		rect(&f, 22, 68, 250, 60, id%2 == 1)
	case 2:
		// Original procedural scene, sampled from real elapsed time. No media
		// files or low-FPS source sequence can constrain this test.
		for i := 0; i < 5; i++ {
			x := 32 + int(math.Mod(t*42+float64(i*51), 228))
			y := 94 + int(17*math.Sin(t*2+float64(i)))
			circle(&f, x, y, 7+i%3)
		}
		rect(&f, 22, 126, 250, 2, true)
	}
	text(&f, 4, 133, "W:700 E:500 R:300 MS  M:MODE H:HELP", 1)
	text(&f, 4, 144, "P:PAUSE C:CLEAN X:EXIT  "+logLabel, 1)
	return f
}
func helpFrame() frame {
	var f frame
	text(&f, 4, 3, "REFRESH TEST", 2)
	lines := []string{
		"Q:1000 W:700 E:500 R:300 T:200 Y:100 MS",
		"RIGHT / + : FASTER BY 50 MS",
		"LEFT / -  : SLOWER BY 50 MS",
		"M / TAB: MOVE / FLIP / SCENE",
		"SPACE / OK / P: PAUSE / RESUME",
		"C: NEW SEGMENT AND FULL CLEAN",
		"H: CLOSE HELP / STAY PAUSED",
		"X / BACK / HOME / ESC: EXIT",
		"WAIT IS SOFTWARE DELAY / NOT PANEL FPS",
		"FILM 20-30S PER SETTING / KEEP CSV",
		"FAST FLASHES: STOP IF UNCOMFORTABLE",
	}
	for i, line := range lines {
		text(&f, 4, 24+i*11, line, 1)
	}
	return f
}
