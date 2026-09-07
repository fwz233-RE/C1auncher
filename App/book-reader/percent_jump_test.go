package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"c1device"
)

func keyO() c1device.Event { return c1device.Event{Key: c1device.KeyRune, Rune: 'o'} }
func inputText(app *readerApp, text string) {
	for _, character := range text {
		app.handleEvent(c1device.Event{Key: c1device.KeyRune, Rune: character})
	}
}

func chapterJumpFixture(t *testing.T) *readerApp {
	t.Helper()
	app := readingFixture(t)
	var text strings.Builder
	for index := 1; index <= 2337; index++ {
		fmt.Fprintf(&text, "第%d章 示例章节\n这一章的第一行正文。\n这一章的第二行正文。\n", index)
	}
	if err := os.WriteFile(app.document.Path, []byte(text.String()), 0644); err != nil {
		t.Fatal(err)
	}
	document, err := OpenDocument(app.document.Path)
	if err != nil {
		t.Fatal(err)
	}
	if len(document.Chapters) != 2337 {
		t.Fatalf("fixture chapters = %d", len(document.Chapters))
	}
	app.document, app.pages, app.view, app.dirty = document, nil, viewChapters, false
	return app
}

func TestChapterOOpensPercentJumpWithoutChangingPosition(t *testing.T) {
	app := chapterJumpFixture(t)
	app.chapterPick = 1000
	app.handleEvent(keyO())
	if app.view != viewPercentJump || app.percentOrigin != viewChapters {
		t.Fatalf("view=%v origin=%v", app.view, app.percentOrigin)
	}
	if app.chapterPick != 1000 || app.chapterIndex != 0 || app.dirty {
		t.Fatal("opening jump changed reading state")
	}
	want := int(app.document.Chapters[1000].Start*100/app.document.Size) * percentScale
	if app.selectedPercent() != want {
		t.Fatalf("default percent=%d want=%d", app.selectedPercent(), want)
	}
	app.handleEvent(c1device.Event{Key: c1device.KeyRune, Rune: 'o', Repeat: true})
	if app.view != viewPercentJump || app.percentInput != "" {
		t.Fatal("held O entered a digit or confirmed")
	}
}

func TestOKReadsChapterWhileOOpensJump(t *testing.T) {
	app := chapterJumpFixture(t)
	app.chapterPick = 37
	app.handle(c1device.KeyOK)
	if app.view != viewReader || app.chapterIndex != 37 {
		t.Fatal("OK did not read selected chapter")
	}
	app.handleEvent(c1device.Event{Key: c1device.KeyRune, Rune: 'O'})
	if app.view != viewPercentJump {
		t.Fatal("O did not open reader jump")
	}
	inputText(app, "19.65")
	app.handle(c1device.KeyOK)
	if app.view != viewReader {
		t.Fatal("OK did not confirm jump")
	}
	app.handleEvent(c1device.Event{Key: c1device.KeyOK, Repeat: true})
	if app.view != viewReader {
		t.Fatal("held confirmation changed view")
	}
	app.handle(c1device.KeyOK)
	if app.view != viewReader {
		t.Fatal("OK reopened jump instead of staying in reader")
	}
}

func TestPercentJumpAdjustClampAndCancel(t *testing.T) {
	for _, cancel := range []c1device.Key{c1device.KeyLeft, c1device.KeyBack} {
		app := chapterJumpFixture(t)
		app.handleEvent(keyO())
		app.handle(c1device.KeyDown)
		if app.selectedPercent() != 0 {
			t.Fatal("percentage below zero")
		}
		app.handle(c1device.KeyUp)
		app.handleEvent(c1device.Event{Key: c1device.KeyUp, Repeat: true})
		if app.selectedPercent() != 600 {
			t.Fatalf("short + long up = %d", app.selectedPercent())
		}
		inputText(app, "19.65")
		app.handle(c1device.KeyUp)
		if app.selectedPercent() != 2065 {
			t.Fatal("rocker lost fractional digits")
		}
		app.handle(c1device.KeyVolumeDown)
		if app.selectedPercent() != 1965 {
			t.Fatal("volume adjustment lost fractional digits")
		}
		app.percentValue = 9999
		app.handleEvent(c1device.Event{Key: c1device.KeyUp, Repeat: true})
		if app.selectedPercent() != 10000 {
			t.Fatal("percentage above 100")
		}
		app.handle(cancel)
		if app.view != viewChapters || app.chapterPick != 0 || app.dirty {
			t.Fatal("cancel changed reading state")
		}
	}
}

func TestPercentInputParsing(t *testing.T) {
	for _, tc := range []struct {
		text string
		want int
		ok   bool
	}{
		{"0", 0, true}, {"100", 10000, true}, {"100.00", 10000, true}, {"19.65", 1965, true}, {"99.99", 9999, true},
		{"1.2", 120, true}, {"0.01", 1, true}, {"19.", 1900, true}, {"", 0, false}, {"137", 0, false}, {"100.01", 0, false},
		{"19.651", 0, false}, {"1..2", 0, false}, {"NaN", 0, false}, {"-1", 0, false}, {"1e2", 0, false}, {"1234567", 0, false},
	} {
		got, ok := parsePercentInput(tc.text)
		if ok != tc.ok || (ok && got != tc.want) {
			t.Errorf("parse %q = %d,%v want=%d,%v", tc.text, got, ok, tc.want, tc.ok)
		}
	}
}

func TestTopRowPrintedDigitsAndDot(t *testing.T) {
	app := chapterJumpFixture(t)
	app.handleEvent(keyO())
	// Q=1 O=9 Z='.' Y=6 T=5, no shift required in this dialog.
	inputText(app, "qozyt")
	if app.percentInput != "19.65" || app.selectedPercent() != 1965 || app.view != viewPercentJump {
		t.Fatalf("top row input=%q", app.percentInput)
	}
	app.handle(c1device.KeyOK)
	if app.view != viewReader {
		t.Fatal("top row input did not jump")
	}
	app.handleEvent(keyO())
	inputText(app, "oozoo")
	if app.percentInput != "99.99" || app.view != viewPercentJump {
		t.Fatal("O confirmed instead of entering nine")
	}
	app.handleEvent(c1device.Event{Key: c1device.KeyRune, Rune: '\b'})
	if app.percentInput != "99.9" {
		t.Fatal("delete did not erase digit")
	}
	app.handle(c1device.KeyPause)
	if app.percentInput != "99.90" {
		t.Fatal("P did not input zero")
	}
	app.handle(c1device.KeyRight)
	if app.view != viewReader {
		t.Fatal("right confirmation failed")
	}
}

func TestPercentJumpNumericEditingAndValidation(t *testing.T) {
	app := chapterJumpFixture(t)
	app.handleEvent(keyO())
	inputText(app, "137")
	app.handle(c1device.KeyOK)
	if app.view != viewPercentJump || app.message == "" || app.dirty {
		t.Fatal("invalid percentage accepted")
	}
	app.handle(c1device.KeyUp)
	if app.percentInput != "137" {
		t.Fatal("invalid percentage silently clamped")
	}
	inputText(app, "\b")
	if app.selectedPercent() != 1300 {
		t.Fatal("delete did not erase last digit")
	}
	inputText(app, ".65")
	inputText(app, "7")
	if app.percentInput != "13.65" || app.message == "" {
		t.Fatal("third decimal not rejected")
	}
	app.handle(c1device.KeyOK)
	if app.view != viewReader || app.percentInput != "" {
		t.Fatal("corrected percentage failed")
	}
	app.handleEvent(keyO())
	inputText(app, ".01")
	if app.percentInput != "0.01" {
		t.Fatal("leading decimal failed")
	}
	inputText(app, "\b\b\b\b")
	if app.percentText() != "0.00" {
		t.Fatal("erasing all digits did not reset draft")
	}
}

func TestPercentJumpConfirmsContentPositionAndSaves(t *testing.T) {
	for _, units := range []int{0, 1, 100, 1965, 3700, 5000, 9900, 9999, 10000} {
		t.Run(fmt.Sprint(units), func(t *testing.T) {
			app := chapterJumpFixture(t)
			app.handleEvent(keyO())
			inputText(app, formatPercentUnits(units))
			wantChapter, wantOffset, ok := positionForPercentUnits(app.document, units)
			if !ok {
				t.Fatal("missing position")
			}
			// Independent integer calculation verifies actual hundredth-percent
			// scaling, not just the same helper on both sides of the assertion.
			expectedByte := app.document.Size * int64(units) / 10000
			if expectedByte == app.document.Size {
				expectedByte--
			}
			if wantOffset != expectedByte {
				t.Fatalf("byte=%d want=%d", wantOffset, expectedByte)
			}
			aligned, err := app.document.AlignLineStart(wantOffset, app.document.Chapters[wantChapter].Start)
			if err != nil {
				t.Fatal(err)
			}
			app.handle(c1device.KeyOK)
			if app.view != viewReader || app.chapterIndex != wantChapter || app.windowStart != aligned || !app.dirty {
				t.Fatal("jump wrong position or view")
			}
			if !validPosition(app.document, app.chapterIndex, app.pages[app.pageIndex].Start) {
				t.Fatal("invalid page")
			}
			if err := app.saveProgress(); err != nil {
				t.Fatal(err)
			}
			saved, ok, err := app.store.Load(app.document.Path)
			if err != nil || !ok || saved.Chapter != wantChapter || saved.Offset != app.pages[app.pageIndex].Start {
				t.Fatal("saved progress mismatch")
			}
			app.handle(c1device.KeyLeft)
			if app.view != viewChapters || app.chapterPick != wantChapter {
				t.Fatal("jump returned to wrong chapter")
			}
		})
	}
}

func TestPercentJumpFailureKeepsDraftAndReadingState(t *testing.T) {
	app := chapterJumpFixture(t)
	app.handleEvent(keyO())
	inputText(app, "19.65")
	if err := os.Remove(app.document.Path); err != nil {
		t.Fatal(err)
	}
	app.handle(c1device.KeyOK)
	if app.view != viewPercentJump || app.message == "" || app.chapterIndex != 0 || app.dirty || app.percentInput != "19.65" {
		t.Fatal("failed jump changed state or lost draft")
	}
}

func TestBookmarkReaderReturnHintAndPercentJumpOrigin(t *testing.T) {
	app := readingFixture(t)
	app.view = viewBookmarks
	app.bookmarks = []Bookmark{{Chapter: 0, Offset: 11}}
	app.handle(c1device.KeyOK)
	if app.view != viewReader || !strings.HasPrefix(app.readerHint(), "←书签") {
		t.Fatal("bookmark reader origin lost")
	}
	app.handleEvent(keyO())
	app.handle(c1device.KeyLeft)
	if app.view != viewReader || app.readerOrigin != viewBookmarks {
		t.Fatal("cancel lost bookmark origin")
	}
	app.handleEvent(keyO())
	inputText(app, "50.01")
	app.handle(c1device.KeyOK)
	if app.view != viewReader || !strings.HasPrefix(app.readerHint(), "←书签") {
		t.Fatal("jump lost bookmark return")
	}
	app.handle(c1device.KeyLeft)
	if app.view != viewBookmarks {
		t.Fatal("left did not return to bookmarks")
	}
}

func TestPercentZeroSupportsUTF8BOM(t *testing.T) {
	app := readingFixture(t)
	path := filepath.Join(t.TempDir(), "bom.txt")
	if err := os.WriteFile(path, []byte("\xef\xbb\xbf第一章 开始\n正文内容\n"), 0644); err != nil {
		t.Fatal(err)
	}
	document, err := OpenDocument(path)
	if err != nil {
		t.Fatal(err)
	}
	app.document = document
	if !app.jumpToPercent(0) || app.windowStart != 3 {
		t.Fatalf("BOM jump failed: %q", app.message)
	}
}

func TestEmptyBookJumpDoesNotOpenDraft(t *testing.T) {
	app := &readerApp{view: viewChapters, document: &Document{Chapters: []Chapter{{Title: "正文"}}}}
	app.handleEvent(keyO())
	if app.view != viewChapters || app.message == "" {
		t.Fatal("empty book opened jump")
	}
}
