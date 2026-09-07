package main

import (
	"image"
	"path/filepath"
	"strings"
	"time"

	"c1device"
)

// The 296 x 152 panel uses fixed, non-overlapping zones. In particular the
// footer starts BELOW the artwork, never inverts it, and has no filled bar.
var (
	musicCoverRect  = image.Rect(10, 26, 108, 124)
	musicFooterRect = image.Rect(10, 129, 286, 151)
)

const (
	musicContentLeft   = 122
	musicContentRight  = 286
	musicTitleWidth    = musicContentRight - musicContentLeft
	musicTitleTop      = 39
	musicTitleLineStep = 21
	// Chinese needs enough physical pixels to preserve strokes on this panel.
	// Keep these sizes shared by the device, previews and layout tests.
	musicTitleFontSize = 18
	musicLabelFontSize = 16
	musicTextThreshold = 96
)

type appState struct {
	tracks     []Track
	selected   int
	current    int
	activeGen  uint64
	playing    bool
	paused     bool
	shuffle    bool
	order      *playbackOrder
	position   time.Duration
	duration   time.Duration
	volume     int
	notice     string
	cover      image.Image
	visual     visualMode
	visualTick uint8
}

func renderMusic(state appState, face, small *c1device.Face) c1device.Frame {
	canvas := c1device.NewCanvas()
	// Small wordmark and fine rules instead of dense black chrome.
	drawMusicMark(canvas, 10, 5)
	drawSmallText(canvas, small, 26, -2, "MUSIC")
	drawSmallText(canvas, small, 122, -2, musicVisualLabel(state.visual))
	drawSmallTextRight(canvas, small, 286, -2, trackPositionLabel(state))
	canvas.DrawLine(10, 20, 285, 20)

	cover := state.cover
	if cover == nil {
		cover = defaultCover()
	}
	switch state.visual {
	case visualBars:
		cover = barsArtwork(visualPhase(state), state.playing && !state.paused)
	case visualRecord:
		cover = recordArtwork(visualPhase(state), true)
	}
	canvas.DrawImage(cover, musicCoverRect)

	drawPlaybackGlyph(canvas, musicContentLeft, 28, state.playing, state.paused)
	drawSmallText(canvas, small, 136, 20, playbackStatus(state))
	mode := "顺序"
	if state.shuffle {
		mode = "随机"
	}
	drawSmallTextRight(canvas, small, musicContentRight, 20, mode)

	for index, line := range titleLines(face, currentTrackTitle(state), musicTitleWidth) {
		canvas.DrawTextThreshold(face, musicContentLeft, musicTitleTop+index*musicTitleLineStep, line, musicTextThreshold)
	}
	drawSmallText(canvas, small, musicContentLeft, 82, fitText(small, trackDetailLabel(state), musicTitleWidth))
	drawSmallText(canvas, small, musicContentLeft, 102, elapsedTimeLabel(state))
	drawSmallTextRight(canvas, small, musicContentRight, 102, durationLabel(state))
	drawProgress(canvas, state.position, state.duration, musicContentLeft, 123, musicTitleWidth, 3)

	canvas.DrawLine(10, 128, 285, 128)
	if state.notice != "" {
		drawSmallText(canvas, small, 10, 129, fitText(small, state.notice, musicFooterRect.Dx()))
	} else {
		drawSmallText(canvas, small, 10, 129, controlHint(state))
		drawVolumeGlyph(canvas, 250, 138)
		drawSmallTextRight(canvas, small, 286, 129, formatInteger(state.volume))
	}
	return canvas.Frame(128)
}

func drawSmallText(canvas *c1device.Canvas, face *c1device.Face, x, top int, text string) {
	canvas.DrawTextThreshold(face, x, top, text, musicTextThreshold)
}

func drawSmallTextRight(canvas *c1device.Canvas, face *c1device.Face, right, top int, text string) {
	drawSmallText(canvas, face, right-face.Measure(text), top, text)
}

func controlHint(state appState) string {
	action := "播放"
	if state.playing && !state.paused {
		action = "暂停"
	}
	return "OK " + action + "  P 模式  V 视效"
}

func musicVisualLabel(mode visualMode) string {
	switch mode {
	case visualBars:
		return "律动"
	case visualRecord:
		return "唱片"
	default:
		return "静态"
	}
}

func shouldFullRefresh(initial, wasPlaying, wasPaused, playing, paused bool) bool {
	return initial || wasPlaying && (!playing || !wasPaused && paused)
}

func currentTrackTitle(state appState) string {
	if state.current >= 0 && state.current < len(state.tracks) {
		return state.tracks[state.current].Title
	}
	if state.selected >= 0 && state.selected < len(state.tracks) {
		return state.tracks[state.selected].Title
	}
	return "还没有音乐"
}

// Two deliberate title lines; the last line alone is ellipsized. No marquee
// is used because frequent text animation is unsuitable for this panel.
func titleLines(face *c1device.Face, text string, width int) []string {
	text = strings.Join(strings.Fields(text), " ")
	lines := face.Wrap(text, width)
	if len(lines) == 0 {
		return nil
	}
	if len(lines) > 2 {
		lines = []string{lines[0], fitText(face, strings.Join(lines[1:], ""), width)}
	}
	return lines
}

func trackDetailLabel(state appState) string {
	if len(state.tracks) == 0 {
		return "放入 Music 文件夹即可"
	}
	if state.current >= 0 && state.selected >= 0 && state.selected < len(state.tracks) && state.selected != state.current {
		return "待选 · " + state.tracks[state.selected].Title
	}
	if state.visual != visualStatic {
		return "播放动效 · V 切换"
	}
	index := state.current
	if index < 0 || index >= len(state.tracks) {
		index = state.selected
	}
	if index < 0 || index >= len(state.tracks) {
		return "本地音乐"
	}
	ext := strings.ToUpper(strings.TrimPrefix(filepath.Ext(state.tracks[index].Path), "."))
	if ext == "" {
		return "本地音乐"
	}
	return ext + "  /  本地音乐"
}

func trackPositionLabel(state appState) string {
	if len(state.tracks) == 0 {
		return "00 / 00"
	}
	index := state.selected
	if state.current >= 0 && state.current < len(state.tracks) {
		index = state.current
	}
	if index < 0 || index >= len(state.tracks) {
		index = 0
	}
	return paddedNumber(index+1) + " / " + paddedNumber(len(state.tracks))
}

func paddedNumber(value int) string {
	text := formatInteger(value)
	if value < 10 {
		return "0" + text
	}
	return text
}

func playbackStatus(state appState) string {
	if state.playing {
		if state.paused {
			return "已暂停"
		}
		return "播放中"
	}
	return "待播放"
}

func elapsedTimeLabel(state appState) string {
	position := state.position
	if state.duration > 0 && position > state.duration {
		position = state.duration
	}
	return formatPlaybackTime(position)
}

func durationLabel(state appState) string {
	if state.duration <= 0 {
		return "--:--"
	}
	return formatPlaybackTime(state.duration)
}

func drawMusicMark(canvas *c1device.Canvas, x, y int) {
	for i, height := range []int{4, 10, 7, 3} {
		canvas.FillRect(image.Rect(x+i*3, y+(10-height)/2, x+i*3+1, y+(10+height)/2))
	}
}

func drawPlaybackGlyph(canvas *c1device.Canvas, x, y int, playing, paused bool) {
	if paused {
		canvas.FillRect(image.Rect(x, y, x+2, y+8))
		canvas.FillRect(image.Rect(x+5, y, x+7, y+8))
		return
	}
	if !playing {
		canvas.DrawRect(image.Rect(x, y+1, x+7, y+8))
		return
	}
	for dx := 0; dx < 7; dx++ {
		half := (6 - dx) / 2
		canvas.DrawLine(x+dx, y+4-half, x+dx, y+4+half)
	}
}

func drawVolumeGlyph(canvas *c1device.Canvas, x, y int) {
	canvas.DrawRect(image.Rect(x, y+2, x+3, y+6))
	canvas.DrawLine(x+2, y+2, x+5, y)
	canvas.DrawLine(x+5, y, x+5, y+7)
	canvas.DrawLine(x+5, y+7, x+2, y+5)
}

func drawProgress(canvas *c1device.Canvas, position, duration time.Duration, left, top, width, height int) {
	canvas.DrawLine(left, top+height/2, left+width-1, top+height/2)
	if duration <= 0 || position <= 0 {
		return
	}
	if position > duration {
		position = duration
	}
	filled := int(float64(width-1) * float64(position) / float64(duration))
	canvas.FillRect(image.Rect(left, top+1, left+filled+1, top+height-1))
	x := left + filled
	canvas.FillRect(image.Rect(x, top, x+1, top+height))
}

func fitText(face *c1device.Face, text string, width int) string {
	if face.Measure(text) <= width {
		return text
	}
	runes := []rune(text)
	for len(runes) > 0 {
		runes = runes[:len(runes)-1]
		candidate := strings.TrimSpace(string(runes)) + "…"
		if face.Measure(candidate) <= width {
			return candidate
		}
	}
	return ""
}

func formatInteger(value int) string {
	if value <= 0 {
		return "0"
	}
	var data [20]byte
	position := len(data)
	for value > 0 {
		position--
		data[position] = byte('0' + value%10)
		value /= 10
	}
	return string(data[position:])
}
