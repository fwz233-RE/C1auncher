package main

import (
	"os"
	"path/filepath"
	"testing"

	"c1device"
)

func TestChapterListLongPressSkipsFiveChapters(t *testing.T) {
	chapters := make([]Chapter, 20)
	app := &readerApp{
		view:        viewChapters,
		document:    &Document{Chapters: chapters},
		chapterPick: 0,
	}

	app.handleEvent(c1device.Event{Key: c1device.KeyDown, Repeat: true})
	if app.chapterPick != 5 {
		t.Fatalf("long down selected chapter %d, want 5", app.chapterPick)
	}
	app.handleEvent(c1device.Event{Key: c1device.KeyUp, Repeat: false})
	if app.chapterPick != 4 {
		t.Fatalf("short up selected chapter %d, want 4", app.chapterPick)
	}
}
func TestReaderKeysUseLeftForChaptersAndVerticalPaging(t *testing.T) {
	app := &readerApp{
		view:         viewReader,
		document:     &Document{Size: 100, Chapters: []Chapter{{Title: "正文", Start: 0, End: 100}}},
		pages:        []Page{{Start: 0, End: 10}, {Start: 10, End: 20}, {Start: 20, End: 30}},
		pageIndex:    1,
		chapterIndex: 0,
		windowEnd:    100,
	}

	app.handle(c1device.KeyUp)
	if app.view != viewReader || app.pageIndex != 0 {
		t.Fatalf("up: view=%v page=%d", app.view, app.pageIndex)
	}
	app.handle(c1device.KeyDown)
	if app.view != viewReader || app.pageIndex != 1 {
		t.Fatalf("down: view=%v page=%d", app.view, app.pageIndex)
	}
	app.handle(c1device.KeyLeft)
	if app.view != viewChapters || app.chapterPick != 0 {
		t.Fatalf("left: view=%v chapter=%d", app.view, app.chapterPick)
	}
}

func TestReaderBookmarkToggleAndBookmarkListEntry(t *testing.T) {
	root := t.TempDir()
	book := filepath.Join(root, "book.txt")
	if err := os.WriteFile(book, []byte("0123456789"), 0o644); err != nil {
		t.Fatal(err)
	}
	app := &readerApp{
		view:          viewReader,
		document:      &Document{Path: book, Size: 10, Chapters: []Chapter{{Title: "正文", Start: 0, End: 10}}},
		pages:         []Page{{Start: 3, End: 8}},
		chapterIndex:  0,
		bookmarkStore: BookmarkStore{Dir: filepath.Join(root, "state")},
	}

	app.handle(c1device.KeyRight)
	if app.message != "" {
		t.Fatalf("adding bookmark produced a message: %q", app.message)
	}
	if len(app.bookmarks) != 1 || app.bookmarks[0] != (Bookmark{Chapter: 0, Offset: 3}) {
		t.Fatalf("added bookmarks = %#v", app.bookmarks)
	}
	persisted, err := app.bookmarkStore.Load(book)
	if err != nil || len(persisted) != 1 {
		t.Fatalf("persisted bookmarks = %#v, err=%v", persisted, err)
	}
	app.handle(c1device.KeyRight)
	if app.message != "" {
		t.Fatalf("deleting bookmark produced a message: %q", app.message)
	}
	if len(app.bookmarks) != 0 {
		t.Fatalf("bookmark was not removed: %#v", app.bookmarks)
	}

	app.view = viewChapters
	app.handle(c1device.KeyPause)
	if app.view != viewBookmarks {
		t.Fatalf("pause from chapters opened view %v", app.view)
	}
}

func TestBookmarkLabelsKeepPercentageForJumpTarget(t *testing.T) {
	app := &readerApp{
		document: &Document{
			Size:     100,
			Chapters: []Chapter{{Title: "正文", Start: 0, End: 100}},
		},
		bookmarks: []Bookmark{{Chapter: 0, Offset: 37}},
	}

	labels := app.bookmarkLabels()
	if len(labels) != 1 || labels[0] != "正文  37.0%" {
		t.Fatalf("bookmark labels = %#v", labels)
	}
}

func TestCurrentProgressPreservesResumeBeforeOpeningPage(t *testing.T) {
	book := filepath.Join(t.TempDir(), "book.txt")
	if err := os.WriteFile(book, []byte("0123456789"), 0o644); err != nil {
		t.Fatal(err)
	}
	app := &readerApp{
		document:     &Document{Path: book, Size: 10, Chapters: []Chapter{{Start: 0, End: 10}}},
		chapterIndex: 0,
		resumeOffset: 7,
	}
	progress, ok := app.currentProgress()
	if !ok || progress.Offset != 7 {
		t.Fatalf("progress=%#v ok=%v", progress, ok)
	}
}

func TestDeleteSelectedBookmarkKeepsSelectionInRange(t *testing.T) {
	root := t.TempDir()
	book := filepath.Join(root, "book.txt")
	if err := os.WriteFile(book, []byte("0123456789"), 0o644); err != nil {
		t.Fatal(err)
	}
	app := &readerApp{
		view:          viewBookmarks,
		document:      &Document{Path: book, Size: 10, Chapters: []Chapter{{Title: "正文", Start: 0, End: 10}}},
		bookmarks:     []Bookmark{{Chapter: 0, Offset: 2}, {Chapter: 0, Offset: 7}},
		bookmarkPick:  1,
		bookmarkStore: BookmarkStore{Dir: filepath.Join(root, "state")},
	}

	app.handle(c1device.KeyPause)
	if len(app.bookmarks) != 1 || app.bookmarkPick != 0 {
		t.Fatalf("bookmarks=%#v pick=%d", app.bookmarks, app.bookmarkPick)
	}
}

func TestPositionForPercentUsesChapterAndByteOffset(t *testing.T) {
	document := &Document{
		Size: 100,
		Chapters: []Chapter{
			{Start: 0, End: 40},
			{Start: 40, End: 100},
		},
	}

	tests := []struct {
		percent int
		chapter int
		offset  int64
	}{
		{percent: 0, chapter: 0, offset: 0},
		{percent: 40, chapter: 1, offset: 40},
		{percent: 100, chapter: 1, offset: 99},
	}
	for _, test := range tests {
		chapter, offset, ok := positionForPercent(document, test.percent)
		if !ok || chapter != test.chapter || offset != test.offset {
			t.Fatalf("positionForPercent(%d) = (%d, %d, %v), want (%d, %d, true)", test.percent, chapter, offset, ok, test.chapter, test.offset)
		}
	}
	if _, _, ok := positionForPercent(document, 101); ok {
		t.Fatal("out-of-range percentage was accepted")
	}
}

func TestReaderOnlyOOpensNumericDraft(t *testing.T) {
	app := readingFixture(t)
	inputText(app, "37")
	if app.view != viewReader || app.percentInput != "" {
		t.Fatal("reader unexpectedly started hidden numeric input")
	}
	app.handleEvent(keyO())
	inputText(app, "37.25")
	if app.view != viewPercentJump || app.percentInput != "37.25" {
		t.Fatal("O did not open visible numeric draft")
	}
}

func TestReaderReturnsToBookmarkListAfterBookmarkJump(t *testing.T) {
	app := &readerApp{
		view:         viewBookmarks,
		bookmarks:    []Bookmark{{Chapter: 0, Offset: 3}},
		bookmarkPick: 0,
	}

	app.handle(c1device.KeyRight)
	if app.readerOrigin != viewBookmarks {
		t.Fatalf("reader origin = %v, want bookmarks", app.readerOrigin)
	}
	app.view = viewReader
	app.handle(c1device.KeyLeft)
	if app.view != viewBookmarks {
		t.Fatalf("left from bookmark reader returned to %v, want bookmarks", app.view)
	}
}

func TestListHintsPutBackBeforeForward(t *testing.T) {
	if chapterListHint != "←返回  ↑上移  ↓下移  →阅读" {
		t.Fatalf("chapter hint = %q", chapterListHint)
	}
	if bookmarkListHint != "←返回  ↑上移  ↓下移  →跳转" {
		t.Fatalf("bookmark hint = %q", bookmarkListHint)
	}
	if readerFooterPrefix != "←章节  ↑上页  ↓下页  " {
		t.Fatalf("reader footer prefix = %q", readerFooterPrefix)
	}
}

func TestReaderHintShowsBookmarkActionForCurrentPage(t *testing.T) {
	app := &readerApp{
		document:     &Document{Chapters: []Chapter{{Start: 0, End: 20}}},
		pages:        []Page{{Start: 5, End: 10}},
		pageIndex:    0,
		chapterIndex: 0,
	}
	if got := app.readerHint(); got != "←章节  ↑上页  ↓下页  →书签" {
		t.Fatalf("hint without bookmark = %q", got)
	}

	app.bookmarks = []Bookmark{{Chapter: 0, Offset: 5}}
	if got := app.readerHint(); got != "←章节  ↑上页  ↓下页  →删除" {
		t.Fatalf("hint with bookmark = %q", got)
	}
}
