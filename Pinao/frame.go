package main

import (
	"fmt"
	"image"
	"image/color"
	"image/png"
	"io"
	"time"
	"unicode"
)

const width, height = 296, 152

type frame [width * height / 8]byte

func (f *frame) pixel(x, y int, black bool) {
	if x < 0 || x >= width || y < 0 || y >= height {
		return
	}
	i := (y/8)*width + x
	b := byte(0x80 >> uint(y&7))
	if black {
		f[i] |= b
	} else {
		f[i] &= ^b
	}
}
func (f *frame) box(x, y, w, h int, black bool) {
	for yy := max(0, y); yy < min(height, y+h); yy++ {
		for xx := max(0, x); xx < min(width, x+w); xx++ {
			f.pixel(xx, yy, black)
		}
	}
}
func (f *frame) rect(x, y, w, h int) {
	f.box(x, y, w, 1, true)
	f.box(x, y+h-1, w, 1, true)
	f.box(x, y, 1, h, true)
	f.box(x+w-1, y, 1, h, true)
}
func (f *frame) text(x, y int, s string, scale int, black bool) {
	for _, r := range s {
		glyph, ok := glyphs[unicode.ToUpper(r)]
		if !ok {
			glyph = glyphs['?']
		}
		for row, bits := range glyph {
			for col := 0; col < 5; col++ {
				if bits&(1<<uint(col)) != 0 {
					f.box(x+col*scale, y+row*scale, scale, scale, black)
				}
			}
		}
		x += 6 * scale
	}
}
func (f *frame) line(x0, y0, x1, y1 int) {
	dx := abs(x1 - x0)
	sx := -1
	if x0 < x1 {
		sx = 1
	}
	dy := -abs(y1 - y0)
	sy := -1
	if y0 < y1 {
		sy = 1
	}
	err := dx + dy
	for {
		f.pixel(x0, y0, true)
		if x0 == x1 && y0 == y1 {
			break
		}
		e2 := 2 * err
		if e2 >= dy {
			err += dy
			x0 += sx
		}
		if e2 <= dx {
			err += dx
			y0 += sy
		}
	}
}
func abs(n int) int {
	if n < 0 {
		return -n
	}
	return n
}

var toneNames = [3]string{"PIANO", "BELL", "CHIP"}

func render(m *model, now time.Time) frame { return renderView(m.view(now)) }

func renderView(v viewState) frame {
	var f frame
	if v.Management {
		f.box(0, 0, width, 22, true)
		f.text(7, 4, "PINAO", 2, false)
		f.text(83, 4, "POCKET MUSIC STUDIO", 1, false)
		f.text(83, 13, "FWZ233  /  MANAGER", 1, false)
		f.text(7, 28, "MUSIC LIBRARY", 1, true)
		f.text(178, 28, fmt.Sprintf("%02d/%02d", v.ManagerIndex+1, v.ManagerCount), 1, true)
		if v.ManagerCount == 0 {
			f.text(7, 52, "NO ARCHIVES OR EXPORTS", 1, true)
		} else {
			for i := 0; i < len(v.ManagerNames); i++ {
				item := v.ManagerOffset + i
				if item >= v.ManagerCount {
					break
				}
				y := 42 + i*14
				selected := item == v.ManagerIndex
				if selected {
					f.box(5, y-2, 286, 13, true)
				}
				label := "S"
				if i < len(v.ManagerNames) && v.ManagerSelectedWAV && selected {
					label = ">"
				}
				f.text(9, y, label, 1, !selected)
				f.text(24, y, v.ManagerNames[i], 1, !selected)
			}
		}
		f.text(7, 128, "UP/DOWN SELECT  ENTER PREVIEW  O OPEN", 1, true)
		f.text(7, 142, "R NEW  Q SAVE AS  DEL DELETE  M/L CLOSE", 1, true)
		return f
	}
	f.box(0, 0, width, 22, true)
	f.text(7, 4, "PINAO", 2, false)
	f.text(83, 4, "POCKET MUSIC STUDIO", 1, false)
	f.text(83, 13, "FWZ233  /  "+toneNames[v.Tone], 1, false)
	f.pageIndicator(v.Page, v.PageCount)
	f.text(7, 28, fmt.Sprintf("%03d BPM", v.BPM), 1, true)
	state := "LIVE"
	if v.Playing {
		state = "LOOP"
	}
	if v.Recording {
		state = "AUTO REC"
	}
	if v.StepMode {
		state = "STEP"
		if v.Recording {
			state = "STEP REC"
		}
	}
	f.text(111, 28, state, 1, true)
	if v.Step >= 0 {
		f.text(165, 28, fmt.Sprintf("%02d/16", v.Step+1), 1, true)
	}
	f.text(205, 28, fmt.Sprintf("OCT%d V%02d", v.Octave, v.Volume), 1, true)
	if v.Help {
		f.box(0, 24, width, height-24, false)
		lines := []string{
			"PLAY: A W S E D F T G Y H U J K",
			"CVBN: KICK/SNARE/HAT/CLAP  Q: TONE",
			"Z/X: OCTAVE  R: RECORD  SPACE: LOOP",
			"LEFT/RIGHT: STEP  UP/DOWN: BPM",
			"I/O: TAP PAGE / HOLD INSERT",
			"VOL KEYS: VOLUME  ENTER: SAVE  P: WAV",
			"DEL X2: CLEAR  HOME/BACK: EXIT",
			"HOLD NOTE + RIGHT: TIE  L: CLOSE",
		}
		for i, s := range lines {
			f.text(7, 29+i*15, s, 1, true)
		}
		return f
	}
	// Fixed indicators show recent activity, or saved cell contents in step mode.
	labels := [...]string{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B", "C"}
	for i, label := range labels {
		x := 7 + i*22
		f.rect(x, 41, 19, 16)
		active := v.Notes&(1<<uint(i)) != 0
		if active {
			f.box(x+1, 42, 17, 14, true)
		}
		f.text(x+3, 46, label, 1, !active)
	}
	for i, label := range [...]string{"C KICK", "V SNARE", "B HAT", "N CLAP"} {
		x := 7 + i*72
		active := v.Drums&(1<<uint(i)) != 0
		if active {
			f.box(x, 62, 67, 13, true)
		}
		f.text(x+3, 65, label, 1, !active)
	}
	// Eight white keys and five black keys, with physical key legends.
	whites := []struct {
		n     int
		label string
		code  uint16
	}{{0, "A", 30}, {2, "S", 31}, {4, "D", 32}, {5, "F", 33}, {7, "G", 34}, {9, "H", 35}, {11, "J", 36}, {12, "K", 37}}
	for i, k := range whites {
		x := 7 + i*35
		f.rect(x, 80, 36, 39)
		held := v.Keys&(1<<uint(k.n)) != 0
		if held {
			f.box(x+2, 98, 32, 18, true)
		}
		f.text(x+15, 107, k.label, 1, !held)
	}
	blacks := []struct {
		boundary int
		label    string
		code     uint16
	}{{1, "W", 17}, {2, "E", 18}, {4, "T", 20}, {5, "Y", 21}, {6, "U", 22}}
	for _, k := range blacks {
		x := 7 + k.boundary*35 - 9
		f.box(x, 80, 19, 22, true)
		n, _ := keyNote(k.code)
		if v.Keys&(1<<uint(n)) != 0 {
			f.rect(x-2, 78, 23, 26)
			f.box(x+3, 83, 13, 16, false)
			f.text(x+7, 89, k.label, 1, true)
		} else {
			f.text(x+7, 89, k.label, 1, false)
		}
	}
	for i := 0; i < steps; i++ {
		x := 7 + i*18
		f.rect(x, 123, 13, 8)
		if v.Pattern&(1<<uint(i)) != 0 {
			f.box(x+3, 125, 7, 4, true)
		}
		if v.Ties&(1<<uint(i)) != 0 {
			f.box(x-6, 126, 7, 2, true)
		}
		if i == steps-1 && v.TieOut {
			f.box(x+12, 126, 6, 2, true)
		}
		if i == v.Step {
			f.box(x, 133, 13, 2, true)
			if v.StepMode {
				f.rect(x-2, 121, 17, 12)
			}
		}
	}
	f.text(7, 142, v.Footer, 1, true)
	return f
}

// pageIndicator only draws in the unused header area x=217..287, y=3..19.
// Fixed neighbor slots keep the current page centered, including at either end.
func (f *frame) pageIndicator(page, count int) {
	count = max(1, min(64, count)) // A zero-value view still has one page.
	page = max(0, min(count-1, page))
	drawTab := func(x, y, w, h, number int, current bool) {
		f.box(x, y, w, h, false)
		if !current {
			// A one-pixel white outline with a black interior.
			f.box(x+1, y+1, w-2, h-2, true)
		}
		label := fmt.Sprintf("%d", number)
		textWidth := len(label)*6 - 1
		f.text(x+(w-textWidth)/2, y+(h-7)/2, label, 1, current)
	}
	if page > 0 {
		drawTab(217, 6, 19, 11, page, false)
	}
	// The selected tab is taller and wider, with a white fill and black digits.
	drawTab(241, 3, 23, 17, page+1, true)
	if page+1 < count {
		drawTab(269, 6, 19, 11, page+2, false)
	}
}

func writePNG(w io.Writer, f frame, scale int) error {
	if scale < 1 || scale > 8 {
		return fmt.Errorf("invalid scale")
	}
	im := image.NewGray(image.Rect(0, 0, width*scale, height*scale))
	for y := 0; y < height; y++ {
		for x := 0; x < width; x++ {
			c := color.Gray{Y: 255}
			if f[(y/8)*width+x]&(0x80>>uint(y&7)) != 0 {
				c.Y = 0
			}
			for yy := 0; yy < scale; yy++ {
				for xx := 0; xx < scale; xx++ {
					im.SetGray(x*scale+xx, y*scale+yy, c)
				}
			}
		}
	}
	return png.Encode(w, im)
}

var glyphs = map[rune][7]byte{
	'#': {10, 10, 31, 10, 31, 10, 10},
	' ': {}, '-': {0, 0, 0, 31}, '.': {0, 0, 0, 0, 0, 12, 12}, '/': {16, 16, 8, 4, 2, 1, 1}, '+': {0, 4, 4, 31, 4, 4},
	'0': {14, 17, 25, 21, 19, 17, 14}, '1': {4, 6, 4, 4, 4, 4, 14}, '2': {14, 17, 16, 14, 1, 1, 31}, '3': {31, 16, 8, 12, 16, 17, 14},
	'4': {8, 12, 10, 9, 31, 8, 8}, '5': {31, 1, 15, 16, 16, 17, 14}, '6': {28, 2, 1, 15, 17, 17, 14}, '7': {31, 16, 16, 8, 4, 2, 1},
	'8': {14, 17, 17, 14, 17, 17, 14}, '9': {14, 17, 17, 30, 16, 8, 7}, ':': {0, 12, 12, 0, 12, 12}, '?': {14, 17, 16, 12, 4, 0, 4},
	'A': {4, 10, 17, 17, 31, 17, 17}, 'B': {15, 17, 17, 15, 17, 17, 15}, 'C': {14, 17, 1, 1, 1, 17, 14}, 'D': {15, 17, 17, 17, 17, 17, 15},
	'E': {31, 1, 1, 15, 1, 1, 31}, 'F': {31, 1, 1, 15, 1, 1, 1}, 'G': {30, 17, 1, 1, 25, 17, 30}, 'H': {17, 17, 17, 31, 17, 17, 17},
	'I': {14, 4, 4, 4, 4, 4, 14}, 'J': {28, 8, 8, 8, 8, 9, 6}, 'K': {17, 9, 5, 3, 5, 9, 17}, 'L': {1, 1, 1, 1, 1, 1, 31},
	'M': {17, 27, 21, 21, 21, 17, 17}, 'N': {17, 17, 19, 21, 25, 17, 17}, 'O': {14, 17, 17, 17, 17, 17, 14}, 'P': {15, 17, 17, 15, 1, 1, 1},
	'Q': {14, 17, 17, 17, 21, 9, 22}, 'R': {15, 17, 17, 15, 5, 9, 17}, 'S': {14, 17, 1, 14, 16, 17, 14}, 'T': {31, 21, 4, 4, 4, 4, 4},
	'U': {17, 17, 17, 17, 17, 17, 14}, 'V': {17, 17, 17, 17, 17, 10, 4}, 'W': {17, 17, 17, 21, 21, 21, 10}, 'X': {17, 17, 10, 4, 10, 17, 17},
	'Y': {17, 17, 10, 4, 4, 4, 4}, 'Z': {31, 16, 8, 4, 2, 1, 31},
}
