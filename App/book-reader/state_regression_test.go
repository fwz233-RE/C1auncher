package main

import (
	"os"
	"path/filepath"
	"testing"

	"c1device"
	"golang.org/x/image/font/gofont/goregular"
)

func readingFixture(t *testing.T) *readerApp {
	t.Helper()
	root := t.TempDir()
	path := filepath.Join(root, "first.txt")
	if err := os.WriteFile(path, []byte("first line\nsecond line\nthird line\n"), 0644); err != nil {
		t.Fatal(err)
	}
	document, err := OpenDocument(path)
	if err != nil {
		t.Fatal(err)
	}
	typeface, err := c1device.ParseTypeface(goregular.TTF)
	if err != nil {
		t.Fatal(err)
	}
	face, err := typeface.NewFace(readerBodyFontSize)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { face.Close() })
	return &readerApp{document: document, bodyFace: face, view: viewReader,
		pages: []Page{{Start: 11, End: document.Size}}, windowStart: 11, windowEnd: document.Size,
		store: ProgressStore{Dir: filepath.Join(root, "state")}, bookmarkStore: BookmarkStore{Dir: filepath.Join(root, "state")}, dirty: true}
}

func TestChapterRoundTripPreservesReadingPosition(t *testing.T) {
	app := readingFixture(t)
	app.handle(c1device.KeyLeft)
	app.handle(c1device.KeyRight)
	if app.pages[app.pageIndex].Start != 11 {
		t.Fatalf("chapter round trip reset offset to %d", app.pages[app.pageIndex].Start)
	}
}

func TestSwitchBookFlushesPendingProgress(t *testing.T) {
	app := readingFixture(t)
	previous := app.document.Path
	next := filepath.Join(filepath.Dir(previous), "next.txt")
	if err := os.WriteFile(next, []byte("another book\n"), 0644); err != nil {
		t.Fatal(err)
	}
	app.books = []Book{{Path: next}}
	app.openSelectedBook()
	progress, ok, err := app.store.Load(previous)
	if err != nil || !ok || progress.Offset != 11 {
		t.Fatalf("pending progress lost: %+v, %v, %v", progress, ok, err)
	}
}

func TestFailedChapterOpenKeepsPreviousReadingState(t *testing.T) {
	app := readingFixture(t)
	app.document.Chapters = []Chapter{{Start: 0, End: 11}, {Start: 11, End: app.document.Size}}
	app.pages = []Page{{Start: 0, End: 11}}
	if err := os.Remove(app.document.Path); err != nil {
		t.Fatal(err)
	}
	app.openChapter(1, 11)
	if app.message == "" {
		t.Fatal("expected read failure")
	}
	if app.chapterIndex != 0 || app.pages[0].Start != 0 {
		t.Fatalf("failed read changed chapter: %d", app.chapterIndex)
	}
}

func TestFailedSaveRetainsDirtyProgressAndCurrentBook(t *testing.T) {
	app := readingFixture(t)
	previous := app.document
	app.store.Dir = previous.Path // A regular file cannot be a state directory.
	app.books = []Book{{Path: previous.Path}}
	app.openSelectedBook()
	if app.message == "" || !app.dirty || app.document != previous {
		t.Fatalf("save failure lost pending state: message=%q dirty=%v", app.message, app.dirty)
	}
	app.store.Dir = filepath.Join(filepath.Dir(previous.Path), "recovered-state")
	if err := app.saveProgress(); err != nil {
		t.Fatal(err)
	}
	if app.dirty {
		t.Fatal("successful retry did not clear pending progress")
	}
}

func TestRepeatedBookmarkKeyDoesNotUndoBookmark(t *testing.T) {
	app := readingFixture(t)
	app.handleEvent(c1device.Event{Key: c1device.KeyRight})
	app.handleEvent(c1device.Event{Key: c1device.KeyRight, Repeat: true})
	if len(app.bookmarks) != 1 {
		t.Fatalf("held bookmark key toggled again: %v", app.bookmarks)
	}
}
