package main

import (
	"c1device"
	"image"
	"strings"
)

// Four independent cues in physical direction order: left, up, down, right.
// Commands such as P bookmarks/delete live in the header instead.
func (app *readerApp) drawNavigationFooter(canvas *c1device.Canvas, hint string) {
	labels := strings.Fields(hint)
	if len(labels) != 4 {
		return
	}
	x := [4]int{4, 82, 158, 244}
	for i, label := range labels {
		canvas.DrawText(app.uiFace, x[i], 133, label)
	}
	canvas.InvertRect(image.Rect(0, readerListFooterTop, c1device.DisplayWidth, c1device.DisplayHeight))
}
