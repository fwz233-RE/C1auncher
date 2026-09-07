package main

import "unicode"

const (
	displayWidth      = 296
	displayHeight     = 152
	displayFrameBytes = displayWidth * (displayHeight / 8)
)

type frame [displayFrameBytes]byte

type helloState struct {
	lastKey string
	count   uint64
}

func renderHello(state helloState) frame {
	var output frame
	strokeRect(&output, 0, 0, displayWidth, displayHeight)
	drawCenteredText(&output, 18, "HELLO", 2)
	drawCenteredText(&output, 38, "WORLD", 2)
	drawHorizontal(&output, 12, 67, displayWidth-24)
	drawCenteredText(&output, 78, "INDEPENDENT E-PAPER APP", 1)
	drawCenteredText(&output, 96, "KEY: "+state.lastKey, 1)
	drawCenteredText(&output, 110, "EVENTS: "+formatCount(state.count), 1)
	drawCenteredText(&output, 134, "HOME OR BACK: DESKTOP", 1)
	return output
}

func formatCount(value uint64) string {
	if value == 0 {
		return "0"
	}
	var digits [20]byte
	position := len(digits)
	for value > 0 {
		position--
		digits[position] = byte('0' + value%10)
		value /= 10
	}
	return string(digits[position:])
}

func drawCenteredText(output *frame, y int, text string, scale int) {
	width := len([]rune(text))*6*scale - scale
	drawText(output, (displayWidth-width)/2, y, text, scale)
}

func drawText(output *frame, x int, y int, text string, scale int) {
	for _, character := range text {
		drawCharacter(output, x, y, character, scale)
		x += 6 * scale
	}
}

func drawCharacter(output *frame, x int, y int, character rune, scale int) {
	character = unicode.ToUpper(character)
	glyph, exists := helloGlyphs[character]
	if !exists {
		glyph = helloGlyphs['?']
	}
	for row, bits := range glyph {
		for column := 0; column < 5; column++ {
			if bits&(1<<column) == 0 {
				continue
			}
			for dy := 0; dy < scale; dy++ {
				for dx := 0; dx < scale; dx++ {
					setPixel(output, x+column*scale+dx, y+row*scale+dy)
				}
			}
		}
	}
}

func strokeRect(output *frame, x int, y int, width int, height int) {
	drawHorizontal(output, x, y, width)
	drawHorizontal(output, x, y+height-1, width)
	for offset := 0; offset < height; offset++ {
		setPixel(output, x, y+offset)
		setPixel(output, x+width-1, y+offset)
	}
}

func drawHorizontal(output *frame, x int, y int, width int) {
	for offset := 0; offset < width; offset++ {
		setPixel(output, x+offset, y)
	}
}

func setPixel(output *frame, x int, y int) {
	if x < 0 || x >= displayWidth || y < 0 || y >= displayHeight {
		return
	}
	offset := (y/8)*displayWidth + x
	output[offset] |= 0x80 >> (y & 7)
}

var helloGlyphs = map[rune][7]byte{
	' ': {}, '-': {0, 0, 0, 31}, '.': {0, 0, 0, 0, 0, 12, 12},
	'0': {14, 17, 25, 21, 19, 17, 14}, '1': {4, 6, 4, 4, 4, 4, 14},
	'2': {14, 17, 16, 14, 1, 1, 31}, '3': {31, 16, 8, 12, 16, 17, 14},
	'4': {8, 12, 10, 9, 31, 8, 8}, '5': {31, 1, 15, 16, 16, 17, 14},
	'6': {28, 2, 1, 15, 17, 17, 14}, '7': {31, 16, 16, 8, 4, 2, 1},
	'8': {14, 17, 17, 14, 17, 17, 14}, '9': {14, 17, 17, 30, 16, 8, 7},
	':': {0, 12, 12, 0, 12, 12}, '?': {14, 17, 16, 12, 4, 0, 4},
	'A': {4, 10, 17, 17, 31, 17, 17}, 'B': {15, 17, 17, 15, 17, 17, 15},
	'C': {14, 17, 1, 1, 1, 17, 14}, 'D': {15, 17, 17, 17, 17, 17, 15},
	'E': {31, 1, 1, 15, 1, 1, 31}, 'F': {31, 1, 1, 15, 1, 1, 1},
	'G': {30, 17, 1, 1, 25, 17, 30}, 'H': {17, 17, 17, 31, 17, 17, 17},
	'I': {14, 4, 4, 4, 4, 4, 14}, 'J': {28, 8, 8, 8, 8, 9, 6},
	'K': {17, 9, 5, 3, 5, 9, 17}, 'L': {1, 1, 1, 1, 1, 1, 31},
	'M': {17, 27, 21, 21, 21, 17, 17}, 'N': {17, 17, 19, 21, 25, 17, 17},
	'O': {14, 17, 17, 17, 17, 17, 14}, 'P': {15, 17, 17, 15, 1, 1, 1},
	'Q': {14, 17, 17, 17, 21, 9, 22}, 'R': {15, 17, 17, 15, 5, 9, 17},
	'S': {14, 17, 1, 14, 16, 17, 14}, 'T': {31, 21, 4, 4, 4, 4, 4},
	'U': {17, 17, 17, 17, 17, 17, 14}, 'V': {17, 17, 17, 17, 17, 10, 4},
	'W': {17, 17, 17, 21, 21, 21, 10}, 'X': {17, 17, 10, 4, 10, 17, 17},
	'Y': {17, 17, 10, 4, 4, 4, 4}, 'Z': {31, 16, 8, 4, 2, 1, 31},
}
