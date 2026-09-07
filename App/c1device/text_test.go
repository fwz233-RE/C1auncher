package c1device

import (
	"image"
	"image/color"
	"testing"
)

func TestCanvasFrameUsesDeviceBitLayout(t *testing.T) {
	canvas := NewCanvas()
	canvas.setBlack(0, 0)
	canvas.setBlack(DisplayWidth-1, DisplayHeight-1)
	frame := canvas.Frame(128)
	if frame[0] != 0x80 {
		t.Fatalf("top-left pixel = %#x, want 0x80", frame[0])
	}
	last := (DisplayHeight/8-1)*DisplayWidth + DisplayWidth - 1
	if frame[last] != 0x01 {
		t.Fatalf("bottom-right pixel = %#x, want 0x01", frame[last])
	}
}

func TestCanvasInvertRectClipsAndTogglesPixels(t *testing.T) {
	canvas := NewCanvas()
	canvas.setBlack(1, 1)
	canvas.InvertRect(image.Rect(-2, -2, 3, 3))

	if got := canvas.image.GrayAt(1, 1).Y; got != 0xff {
		t.Fatalf("black pixel after inversion = %#x, want 0xff", got)
	}
	if got := canvas.image.GrayAt(2, 2).Y; got != 0x00 {
		t.Fatalf("white pixel after inversion = %#x, want 0x00", got)
	}
	if got := canvas.image.GrayAt(3, 3).Y; got != 0xff {
		t.Fatalf("pixel outside inversion = %#x, want 0xff", got)
	}
}

func TestCanvasDrawImageConvertsToBlackAndWhite(t *testing.T) {
	canvas := NewCanvas()
	source := image.NewGray(image.Rect(0, 0, 2, 2))
	source.SetGray(0, 0, color.Gray{Y: 0})
	source.SetGray(1, 1, color.Gray{Y: 0xff})
	canvas.DrawImage(source, image.Rect(0, 0, 2, 2))

	if got := canvas.image.GrayAt(0, 0).Y; got != 0x00 {
		t.Fatalf("black source pixel = %#x, want 0x00", got)
	}
	if got := canvas.image.GrayAt(1, 1).Y; got != 0xff {
		t.Fatalf("white source pixel = %#x, want 0xff", got)
	}
}

func TestCanvasDrawRectUsesExclusiveBounds(t *testing.T) {
	canvas := NewCanvas()
	canvas.DrawRect(image.Rect(1, 1, 4, 4))

	for _, point := range []image.Point{{1, 1}, {3, 1}, {1, 3}, {3, 3}} {
		if got := canvas.image.GrayAt(point.X, point.Y).Y; got != 0x00 {
			t.Fatalf("border pixel %v = %#x, want 0x00", point, got)
		}
	}
	if got := canvas.image.GrayAt(2, 2).Y; got != 0xff {
		t.Fatalf("rect interior = %#x, want 0xff", got)
	}
}

func TestDrawTextMaskKeepsLightAntialiasCoverage(t *testing.T) {
	canvas := NewCanvas()
	mask := image.NewAlpha(image.Rect(0, 0, 3, 1))
	mask.SetAlpha(0, 0, color.Alpha{A: textCoverageThreshold - 1})
	mask.SetAlpha(1, 0, color.Alpha{A: textCoverageThreshold})
	mask.SetAlpha(2, 0, color.Alpha{A: 0xff})

	canvas.drawTextMask(mask, textCoverageThreshold)

	if got := canvas.image.GrayAt(0, 0).Y; got != 0xff {
		t.Fatalf("coverage below threshold = %#x, want white", got)
	}
	for x := 1; x < 3; x++ {
		if got := canvas.image.GrayAt(x, 0).Y; got != 0x00 {
			t.Fatalf("coverage at x=%d = %#x, want black", x, got)
		}
	}
}

func TestDrawTextMaskHigherThresholdRemovesLightEdges(t *testing.T) {
	canvas := NewCanvas()
	mask := image.NewAlpha(image.Rect(0, 0, 3, 1))
	mask.SetAlpha(0, 0, color.Alpha{A: 63})
	mask.SetAlpha(1, 0, color.Alpha{A: 127})
	mask.SetAlpha(2, 0, color.Alpha{A: 255})

	canvas.drawTextMask(mask, 128)

	for x := 0; x < 2; x++ {
		if got := canvas.image.GrayAt(x, 0).Y; got != 0xff {
			t.Fatalf("light edge at x=%d = %#x, want white", x, got)
		}
	}
	if got := canvas.image.GrayAt(2, 0).Y; got != 0x00 {
		t.Fatalf("solid glyph pixel = %#x, want black", got)
	}
}
