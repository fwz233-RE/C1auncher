package main

import (
	"c1device"
	"fmt"
	"image"
	"testing"
)

func TestNativeFooterWidths(t *testing.T) {
	f, err := newReaderFace(false)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	for _, s := range []string{chapterListHint, bookmarkListHint, percentJumpHint, percentInputHint, "↑↓选择  ←收起  →阅读  P书签", "↑↓选择  →打开  BACK退出"} {
		if w := f.Measure(s); w > 292 {
			t.Errorf("footer width %d: %s", w, s)
		}
	}
}

func TestNativeReadingHeaderDoesNotOverlapPercent(t *testing.T) {
	app := readingFixture(t)
	readerUIFaces(t, app)
	app.document.Chapters[0].Title = "这是一段很长很长的中文章节名称用来验证布局"
	app.pages = []Page{{Start: app.document.Size, End: app.document.Size}}
	rendered := app.render()
	// A long title must stay left of the percentage reservation.
	onlyPercent := c1device.NewCanvas()
	onlyPercent.DrawTextRight(app.uiFace, 290, 1, fmt.Sprintf("%.2f%%", 100.0))
	expected := onlyPercent.Frame(128)
	box := image.Rect(234, 0, 296, 20)
	for y := box.Min.Y; y < box.Max.Y; y++ {
		for x := box.Min.X; x < box.Max.X; x++ {
			if frameBlack(rendered, x, y) != frameBlack(expected, x, y) {
				t.Fatalf("title overlaps percent at %d,%d", x, y)
			}
		}
	}
}
