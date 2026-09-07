package main

import "c1device"

func drawSmallText(canvas *c1device.Canvas, face *c1device.Face, x, top int, text string) {
	canvas.DrawTextThreshold(face, x, top, text, 112)
}

func drawSmallTextRight(canvas *c1device.Canvas, face *c1device.Face, right, top int, text string) {
	drawSmallText(canvas, face, right-face.Measure(text), top, text)
}
