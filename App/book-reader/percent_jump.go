package main

import (
	"fmt"
	"image"
	"strconv"
	"strings"
	"unicode"

	"c1device"
)

// All draft values are integers in 0.01% units. Avoid float rounding and
// truncation of manually entered percentages on the 32-bit device.
const (
	percentScale     = 100
	maxPercentUnits  = 100 * percentScale
	percentJumpHint  = "↑↓调整  ←取消  OK跳转"
	percentInputHint = "顶排数字  Z小数点  DEL删"
)

// The jump screen is a draft: browsing percentages must not change the
// selected chapter, reading position, bookmarks, or saved progress.
func (app *readerApp) openPercentJump() {
	if app.document == nil || app.document.Size <= 0 || len(app.document.Chapters) == 0 {
		app.message = "暂无可跳转的正文"
		return
	}
	if _, ok := app.document.readableChapter(0, 1); !ok {
		app.message = "暂无可跳转的正文"
		return
	}
	offset := int64(0)
	if app.view == viewChapters && app.chapterPick >= 0 && app.chapterPick < len(app.document.Chapters) {
		offset = app.document.Chapters[app.chapterPick].Start
	} else if bookmark, ok := app.currentBookmark(); ok {
		offset = bookmark.Offset
	}
	// Whole-percent default, but fine adjustments entered by the user are
	// retained when the rocker subsequently adds/subtracts one percent.
	app.percentValue = clampPercent(int(float64(offset)*100/float64(app.document.Size)) * percentScale)
	app.percentInput = ""
	app.percentOrigin = app.view
	app.view = viewPercentJump
}

func clampPercent(value int) int {
	if value < 0 {
		return 0
	}
	if value > maxPercentUnits {
		return maxPercentUnits
	}
	return value
}

func parsePercentInput(text string) (int, bool) {
	if text == "" || len(text) > 6 {
		return 0, false
	}
	parts := strings.Split(text, ".")
	if len(parts) > 2 || len(parts[0]) == 0 || len(parts[0]) > 3 {
		return 0, false
	}
	for _, part := range parts {
		for _, digit := range part {
			if digit < '0' || digit > '9' {
				return 0, false
			}
		}
	}
	whole, err := strconv.Atoi(parts[0])
	if err != nil {
		return 0, false
	}
	fraction := 0
	if len(parts) == 2 {
		if len(parts[1]) > 2 {
			return 0, false
		}
		if parts[1] != "" {
			fraction, _ = strconv.Atoi(parts[1])
			if len(parts[1]) == 1 {
				fraction *= 10
			}
		}
	}
	units := whole*percentScale + fraction
	return units, units <= maxPercentUnits
}

func (app *readerApp) selectedPercent() int {
	if app.percentInput != "" {
		units, ok := parsePercentInput(app.percentInput)
		if !ok {
			return -1
		}
		return units
	}
	return app.percentValue
}

func formatPercentUnits(units int) string {
	return fmt.Sprintf("%d.%02d", units/percentScale, units%percentScale)
}

func (app *readerApp) percentText() string {
	if app.percentInput != "" {
		return app.percentInput
	}
	return formatPercentUnits(app.percentValue)
}

func (app *readerApp) inputPercent(character rune) {
	if character == '\b' {
		if app.percentInput == "" {
			app.percentInput = formatPercentUnits(app.percentValue)
		}
		app.percentInput = app.percentInput[:len(app.percentInput)-1]
		if app.percentInput == "" {
			app.percentValue = 0
		}
		return
	}
	if character == '.' {
		if strings.Contains(app.percentInput, ".") {
			return
		}
		if app.percentInput == "" {
			app.percentInput = "0"
		}
		app.percentInput += "."
		return
	}
	if character < '0' || character > '9' {
		return
	}
	if point := strings.IndexByte(app.percentInput, '.'); point >= 0 {
		if len(app.percentInput)-point-1 >= 2 {
			app.message = "最多输入两位小数"
			return
		}
	} else if len(app.percentInput) >= 3 {
		app.message = "百分比范围 0-100"
		return
	}
	app.percentInput += string(character)
}

// In this dialog only, Q-P are the printed 1-0 row. O therefore inputs 9
// and P inputs 0, never confirmation or deletion. Z has the printed dot.
func percentCharacter(event c1device.Event) rune {
	if event.Key == c1device.KeyPause {
		return '0'
	}
	if event.Key != c1device.KeyRune {
		return 0
	}
	character := unicode.ToLower(event.Rune)
	if index := strings.IndexRune("qwertyuiop", character); index >= 0 {
		return rune("1234567890"[index])
	}
	if character == 'z' {
		return '.'
	}
	return character
}

func (app *readerApp) handlePercentJump(event c1device.Event) {
	switch event.Key {
	case c1device.KeyUp, c1device.KeyDown, c1device.KeyVolumeUp, c1device.KeyVolumeDown:
		units := app.selectedPercent()
		if units < 0 {
			app.message = "请先修正输入：0-100"
			return
		}
		delta := chapterSelectionDelta(event) * percentScale
		if event.Key == c1device.KeyDown || event.Key == c1device.KeyVolumeDown {
			delta = -delta
		}
		app.percentValue = clampPercent(units + delta)
		app.percentInput = ""
	case c1device.KeyRune, c1device.KeyPause:
		app.inputPercent(percentCharacter(event))
	case c1device.KeyLeft, c1device.KeyBack:
		app.percentInput = ""
		app.view = app.percentOrigin
	case c1device.KeyOK, c1device.KeyRight:
		percent := app.selectedPercent()
		if percent < 0 || percent > maxPercentUnits {
			app.message = "百分比必须在 0-100 之间"
			return
		}
		if app.jumpToPercentUnits(percent) {
			if app.percentOrigin == viewChapters {
				app.readerOrigin = viewChapters
			}
			app.percentInput = ""
			app.resumeOffset = 0
		}
	}
}

func (app *readerApp) renderPercentJump(canvas *c1device.Canvas) {
	canvas.DrawText(app.uiFace, 6, 1, "百分比跳转")
	canvas.DrawTextRight(app.uiFace, 290, 1, "长按±5%")
	canvas.FillRect(image.Rect(4, readerHeaderBottom-2, 292, readerHeaderBottom))
	canvas.DrawTextCentered(app.uiFace, c1device.DisplayWidth/2, 28, percentInputHint)
	text := app.percentText() + "%"
	canvas.DrawTextInverted(app.bodyFace, image.Rect(90, 51, 206, 79),
		148-app.bodyFace.Measure(text)/2, 53, text)
	chapterIndex, _, ok := positionForPercentUnits(app.document, app.selectedPercent())
	if ok {
		canvas.DrawTextCentered(app.uiFace, 148, 82,
			fitText(app.uiFace, app.document.positionLabel(chapterIndex), 280))
		canvas.DrawTextCentered(app.uiFace, 148, 104,
			fitText(app.uiFace, app.document.Chapters[chapterIndex].Title, 280))
	} else {
		canvas.DrawTextCentered(app.uiFace, 148, 88, "请输入 0-100（最多两位小数）")
	}
	footer := percentJumpHint
	if app.message != "" {
		footer = fitText(app.uiFace, app.message, 280)
	}
	canvas.DrawInvertedTextBar(app.uiFace, image.Rect(0, readerListFooterTop, c1device.DisplayWidth, c1device.DisplayHeight), footer)
}
