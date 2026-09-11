package main

import (
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"testing"

	"c1device"
)

func readerUIFaces(t *testing.T, app *readerApp) {
	t.Helper()
	var err error
	app.uiFace, err = newReaderFace(false)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { app.uiFace.Close() })
	app.bodyFace, err = newReaderFace(true)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { app.bodyFace.Close() })
}

func frameBlack(frame c1device.Frame, x, y int) bool {
	return frame[(y/8)*c1device.DisplayWidth+x]&(0x80>>uint(y&7)) != 0
}

func TestReaderFooterHasNoProgressBar(t *testing.T) {
	app := readingFixture(t)
	readerUIFaces(t, app)
	app.pages = []Page{{Start: 11, End: app.document.Size, Lines: []string{"正文内容"}}}
	frame := app.render()
	for y := readerBodyBottom; y < readerFooterTop; y++ {
		for x := 0; x < c1device.DisplayWidth; x++ {
			if frameBlack(frame, x, y) {
				t.Fatalf("unexpected progress bar pixel at (%d,%d)", x, y)
			}
		}
	}
}

func TestReaderUILayoutAndPreviews(t *testing.T) {
	app := chapterJumpFixture(t)
	readerUIFaces(t, app)
	app.books = []Book{{Name: "蛊真人.txt", Path: app.document.Path}}
	if app.uiFace.Measure("O跳转") > 67 || app.uiFace.Measure("2337 章") > 63 {
		t.Fatal("chapter header button overlaps count")
	}
	for _, hint := range []string{chapterListHint, bookmarkListHint, percentJumpHint,
		"←书签  ↑上页  ↓下页  →删除", percentInputHint} {
		if width := app.uiFace.Measure(hint); width > 292 {
			t.Fatalf("footer %q overflows: %d pixels", hint, width)
		}
	}
	writeReaderPreview(t, "chapters", app.render())
	app.handleEvent(keyO())
	app.percentValue = 3765
	writeReaderPreview(t, "percent-jump", app.render())
	for _, text := range []string{"19.65", "99.99", "100.00"} {
		if width := app.bodyFace.Measure(text + "%"); width > 108 {
			t.Fatalf("percentage input %q overflows: %d pixels", text, width)
		}
		if width := app.uiFace.Measure(text + "%"); width > 60 {
			t.Fatalf("reader percentage %q overlaps title: %d pixels", text, width)
		}
		app.percentInput = text
		writeReaderPreview(t, "percent-input-"+text, app.render())
	}
	app.percentInput = ""
	app.handle(c1device.KeyOK)
	app.readerOrigin = viewBookmarks
	writeReaderPreview(t, "bookmark-reader", app.render())
	app.view = viewBookmarks
	app.bookmarks = nil
	writeReaderPreview(t, "empty-bookmarks", app.render())
}

func TestVolumeUILayoutAndPreviews(t *testing.T) {
	app := volumeFixture(t)
	readerUIFaces(t, app)
	writeReaderPreview(t, "volumes-collapsed", app.render())
	app.chapterPick = 1
	app.handle(c1device.KeyRight)
	writeReaderPreview(t, "volumes-expanded", app.render())
	for _, hint := range []string{app.directoryHint(), "↑↓选择  ←收起  →阅读  P书签"} {
		if width := app.uiFace.Measure(hint); width > 292 {
			t.Fatalf("volume hint overflows: %d", width)
		}
	}
	// Reserve enough space for the count and jump button on the small screen.
	for _, count := range []string{"3卷/4章", "6卷/2337章", "100卷/10000章"} {
		buttonLeft := 290 - app.uiFace.Measure(count) - 8 - 77
		if buttonLeft < 35 {
			t.Fatalf("header overlaps title for %q", count)
		}
	}
	app.handleEvent(keyO())
	app.percentValue = 9000
	writeReaderPreview(t, "volume-percent-jump", app.render())
	app.handle(c1device.KeyOK)
	app.readerOrigin = viewBookmarks
	writeReaderPreview(t, "volume-bookmark-reader", app.render())
}

// Optional local, pixel-exact screenshots use the same 296x152 frame as the
// device. They are test artifacts, never application payload assets.
func writeReaderPreview(t *testing.T, name string, frame c1device.Frame) {
	t.Helper()
	dir := os.Getenv("C1_UI_PREVIEW_DIR")
	if dir == "" {
		return
	}
	if err := os.MkdirAll(dir, 0755); err != nil {
		t.Fatal(err)
	}
	output := image.NewGray(image.Rect(0, 0, c1device.DisplayWidth, c1device.DisplayHeight))
	for y := 0; y < c1device.DisplayHeight; y++ {
		for x := 0; x < c1device.DisplayWidth; x++ {
			value := uint8(255)
			if frameBlack(frame, x, y) {
				value = 0
			}
			output.SetGray(x, y, color.Gray{Y: value})
		}
	}
	file, err := os.Create(filepath.Join(dir, name+".png"))
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	if err := png.Encode(file, output); err != nil {
		t.Fatal(err)
	}
}
