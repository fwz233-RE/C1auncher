package main

// Tiny original bitmap glyphs keep error/help screens independent of font files.
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
}

func pixel(f *frame, x, y int, black bool) {
	if x < 0 || x >= width || y < 0 || y >= height {
		return
	}
	off, mask := (y/8)*width+x, byte(0x80>>uint(y%8))
	if black {
		f[off] |= mask
	} else {
		f[off] &= ^mask
	}
}
func text(f *frame, x, y int, s string, scale int) {
	for _, r := range s {
		g := glyphs[r]
		for row, b := range g {
			for col := 0; col < 5; col++ {
				if b&(1<<uint(4-col)) != 0 {
					for dy := 0; dy < scale; dy++ {
						for dx := 0; dx < scale; dx++ {
							pixel(f, x+col*scale+dx, y+row*scale+dy, true)
						}
					}
				}
			}
		}
		x += 6 * scale
	}
}
func messageFrame(lines ...string) frame {
	var f frame
	for i, s := range lines {
		scale := 1
		if i == 0 {
			scale = 2
		}
		text(&f, 8, 10+i*23, s, scale)
	}
	return f
}
func banner(f *frame, s string) {
	for y := height - 16; y < height; y++ {
		for x := 0; x < width; x++ {
			pixel(f, x, y, false)
		}
	}
	text(f, 4, height-12, s, 1)
}
