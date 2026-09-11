package main

import (
	"fmt"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func chapterPagesFixture(t *testing.T, content string) *readerApp {
	t.Helper()
	app := readingFixture(t)
	path := filepath.Join(t.TempDir(), "chapter-pages.txt")
	if err := os.WriteFile(path, []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	document, err := OpenDocument(path)
	if err != nil {
		t.Fatal(err)
	}
	app.document = document
	app.pages = nil
	app.dirty = false
	return app
}

func assertChapterPage(t *testing.T, app *readerApp, current, total int) {
	t.Helper()
	want := fmt.Sprintf("页 %d/%d", current, total)
	if got := app.chapterPageLabel(); got != want {
		t.Fatalf("page label = %q, want %q (start=%d)", got, want, app.pages[app.pageIndex].Start)
	}
}

func TestChapterPagesCanonicalResumeAndNavigation(t *testing.T) {
	app := chapterPagesFixture(t, "第一章 页码\n"+strings.Repeat("abcdefghijklmnopqrstuvwxyz\n", 101))
	chapter := app.document.Chapters[0]
	canonical, _, err := Paginate(app.document, chapter, chapter.Start, app.bodyFace, readerTextWidth, readerBodyHeight)
	if err != nil {
		t.Fatal(err)
	}
	if len(canonical) < 10 {
		t.Fatal("fixture has too few pages")
	}
	if !app.openChapter(0, chapter.Start) {
		t.Fatal(app.message)
	}
	assertChapterPage(t, app, 1, len(canonical))
	cache := app.chapterPagination
	for page := range canonical {
		if !app.openChapter(0, canonical[page].Start) {
			t.Fatal(app.message)
		}
		assertChapterPage(t, app, page+1, len(canonical))
		if app.chapterPagination != cache {
			t.Fatal("reopening the active chapter rebuilt its page index")
		}
	}

	// This position deliberately is neither a source-line nor page boundary.
	resume := canonical[7].Start + 3
	if !app.openChapter(0, resume) {
		t.Fatal(app.message)
	}
	assertChapterPage(t, app, 8, len(canonical))
	if app.pages[0].Start != resume || app.windowStart != resume {
		t.Fatal("resume was snapped back to a canonical page boundary")
	}
	if err := app.saveProgress(); err != nil {
		t.Fatal(err)
	}
	if saved, ok, err := app.store.Load(app.document.Path); err != nil || !ok || saved.Offset != resume {
		t.Fatalf("saved offset changed: %+v %v %v", saved, ok, err)
	}
	app.nextPage()
	assertChapterPage(t, app, 9, len(canonical))
	if app.pages[app.pageIndex].Start != canonical[8].Start {
		t.Fatal("resumed page shifted later canonical page boundaries")
	}
	app.previousPage()
	assertChapterPage(t, app, 8, len(canonical))
	app.previousPage()
	assertChapterPage(t, app, 7, len(canonical))
	app.nextPage()
	assertChapterPage(t, app, 8, len(canonical))
	if app.pages[app.pageIndex].Start != canonical[7].Start {
		t.Fatal("backward then forward did not restore canonical page start")
	}
}

func TestChapterPagesPercentBookmarkAndTransitions(t *testing.T) {
	app := chapterPagesFixture(t, "第一章 前章\n"+strings.Repeat("甲乙丙丁戊己庚辛壬癸\n", 71)+"第二章 后章\n"+strings.Repeat("abcdefghijklmno\n", 48))
	for _, percent := range []int{0, 13, 50, 75, 99, 100} {
		if !app.jumpToPercent(percent) {
			t.Fatal(app.message)
		}
		chapter := app.document.Chapters[app.chapterIndex]
		pages, _, err := Paginate(app.document, chapter, chapter.Start, app.bodyFace, readerTextWidth, readerBodyHeight)
		if err != nil {
			t.Fatal(err)
		}
		current := 1
		for i, page := range pages {
			if page.Start <= app.pages[0].Start {
				current = i + 1
			}
		}
		assertChapterPage(t, app, current, len(pages))
		bookmark, _ := app.currentBookmark()
		app.bookmarks = []Bookmark{bookmark}
		if !app.openChapter(0, app.document.Chapters[0].Start) {
			t.Fatal(app.message)
		}
		app.openSelectedBookmark()
		assertChapterPage(t, app, current, len(pages))
		if got, _ := app.currentBookmark(); got != bookmark {
			t.Fatalf("bookmark offset changed: %v != %v", got, bookmark)
		}
	}
	if !app.openChapter(1, app.document.Chapters[1].Start) {
		t.Fatal(app.message)
	}
	secondTotal := len(app.chapterPagination.starts)
	app.previousPage()
	if app.chapterIndex != 0 {
		t.Fatal("previous page did not enter previous chapter")
	}
	firstTotal := len(app.chapterPagination.starts)
	assertChapterPage(t, app, firstTotal, firstTotal)
	app.nextPage()
	assertChapterPage(t, app, 1, secondTotal)
	if app.chapterIndex != 1 {
		t.Fatal("next page did not enter next chapter")
	}
	app.pageIndex = len(app.pages) - 1
	app.nextPage()
	assertChapterPage(t, app, secondTotal, secondTotal)
}

func TestChapterPagesMultipleWindows(t *testing.T) {
	// A complete 4 MiB window ends in a partial page (131072 lines / 5).
	// Count the same bounded layout as sequential reading, including that page.
	line := strings.Repeat("a", 31) + "\n"
	firstLines := int(maxChapterWindow) / len(line)
	app := chapterPagesFixture(t, strings.Repeat(line, firstLines+37))
	perPage := readerBodyHeight / app.bodyFace.LineHeight()
	firstPages := (firstLines + perPage - 1) / perPage
	lastPages := (37 + perPage - 1) / perPage
	total := firstPages + lastPages
	resume := maxChapterWindow + int64(12*len(line)+3)
	if !app.openChapter(0, resume) {
		t.Fatal(app.message)
	}
	assertChapterPage(t, app, firstPages+12/perPage+1, total)
	if len(app.chapterPagination.windows) != 2 || app.windowStart != resume || app.pages[0].Start != resume {
		t.Fatal("multi-window resume lost its source offset")
	}
	cache := app.chapterPagination
	boundary := cache.windows[1].firstPage
	if boundary != firstPages || cache.windows[0].end != maxChapterWindow {
		t.Fatal("incorrect canonical window boundary")
	}
	if !app.openChapter(0, cache.starts[boundary-1]) {
		t.Fatal(app.message)
	}
	assertChapterPage(t, app, firstPages, total)
	app.nextPage()
	assertChapterPage(t, app, firstPages+1, total)
	if app.pages[app.pageIndex].Start != maxChapterWindow {
		t.Fatal("next skipped the beginning of the second window")
	}
	app.previousPage()
	assertChapterPage(t, app, firstPages, total)
	if app.pages[app.pageIndex].Start != cache.starts[boundary-1] {
		t.Fatal("previous did not return to the actual preceding window page")
	}
	app.nextPage()
	assertChapterPage(t, app, firstPages+1, total)
	if app.chapterPagination != cache {
		t.Fatal("window transition rebuilt the index")
	}
	if len(app.pages) != lastPages {
		t.Fatal("active pages retain text from earlier windows")
	}
	if !app.openChapter(0, app.document.Size-1) {
		t.Fatal(app.message)
	}
	assertChapterPage(t, app, total, total)
	app.nextPage()
	assertChapterPage(t, app, total, total)
}

func TestChapterPageLabelBoundaryMappingAndNoIO(t *testing.T) {
	app := chapterPagesFixture(t, "第一章 页码\n"+strings.Repeat("abcdef\n", 23))
	if !app.openChapter(0, 0) {
		t.Fatal(app.message)
	}
	index := app.chapterPagination
	for page, start := range index.starts {
		if got := index.pageAt(start); got != page {
			t.Fatalf("pageAt(%d)=%d want=%d", start, got, page)
		}
		if page > 0 && index.pageAt(start-1) != page-1 {
			t.Fatal("offset immediately before page boundary belongs to wrong page")
		}
	}
	if index.pageAt(0) != 0 || index.pageAt(app.document.Size-1) != len(index.starts)-1 {
		t.Fatal("heading or final byte mapped incorrectly")
	}
	if err := os.Remove(app.document.Path); err != nil {
		t.Fatal(err)
	}
	assertChapterPage(t, app, 1, len(index.starts))
	app.nextPage()
	assertChapterPage(t, app, 2, len(index.starts))
	app.document = &Document{Chapters: app.document.Chapters}
	if got := app.chapterPageLabel(); got != "页 —/—" {
		t.Fatalf("old document cache leaked: %q", got)
	}
}

func TestChapterPagesFailedCountAndReadPreserveState(t *testing.T) {
	app := chapterPagesFixture(t, "第一章 正常\n"+strings.Repeat("abcdef\n", 32)+"第二章 失败\n"+strings.Repeat("uvwxyz\n", 21))
	if !app.openChapter(0, 0) {
		t.Fatal(app.message)
	}
	app.nextPage()
	app.dirty = false
	before := *app
	// Corrupt the second chapter after the chapter index was opened. Counting
	// must fail even when the requested suffix by itself would decode cleanly.
	bytes, err := os.ReadFile(app.document.Path)
	if err != nil {
		t.Fatal(err)
	}
	second := app.document.Chapters[1]
	bytes[second.Start+int64(len("第二章 失败\n"))] = 0xff
	if err := os.WriteFile(app.document.Path, bytes, 0600); err != nil {
		t.Fatal(err)
	}
	if app.openChapter(1, second.End-7) || app.message == "" {
		t.Fatal("invalid canonical prefix did not fail page counting")
	}
	assertUnchanged := func() {
		t.Helper()
		if app.chapterIndex != before.chapterIndex || app.chapterPagination != before.chapterPagination ||
			!reflect.DeepEqual(app.pages, before.pages) || app.pageIndex != before.pageIndex ||
			app.windowStart != before.windowStart || app.windowEnd != before.windowEnd ||
			app.resumeOffset != before.resumeOffset || app.dirty != before.dirty || app.view != before.view || app.chapterPick != before.chapterPick {
			t.Fatal("failed count/read changed reading state")
		}
	}
	assertUnchanged()
	if err := os.Remove(app.document.Path); err != nil {
		t.Fatal(err)
	}
	if app.openChapter(0, before.pages[before.pageIndex].Start) {
		t.Fatal("cached boundaries concealed a read failure")
	}
	assertUnchanged()
}

func TestChapterPagesLongUTF8LineWindowBoundary(t *testing.T) {
	// With no newline to align to, the 4 MiB limit splits a three-byte rune.
	path := filepath.Join(t.TempDir(), "long-line.txt")
	content := strings.Repeat("甲", int(maxChapterWindow)/3+1000)
	if err := os.WriteFile(path, []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	chapter := Chapter{Title: "正文", End: int64(len(content)), HasBody: true}
	document := &Document{Path: path, Encoding: EncodingUTF8, Size: chapter.End, Chapters: []Chapter{chapter}}
	index, err := countChapterPages(document, chapter, fixedFace{})
	if err != nil {
		t.Fatal(err)
	}
	if len(index.windows) != 2 || index.windows[0].end != maxChapterWindow-1 {
		t.Fatal("long-line window did not preserve complete UTF-8 characters")
	}
	wantTotal := 0
	for _, window := range index.windows {
		runes := int(window.end-window.start) / 3
		lines := (runes + readerTextWidth - 1) / readerTextWidth
		perPage := readerBodyHeight / (fixedFace{}).LineHeight()
		wantTotal += (lines + perPage - 1) / perPage
	}
	if len(index.starts) != wantTotal {
		t.Fatalf("long-line pages=%d want=%d", len(index.starts), wantTotal)
	}
	for _, offset := range []int64{index.windows[0].end - 3, index.windows[1].start, chapter.End - 3} {
		pages, _, err := index.readWindow(offset)
		if err != nil || len(pages) == 0 || pages[0].Start != offset {
			t.Fatalf("long-line resume at %d failed: %v", offset, err)
		}
	}
}

func TestChapterPagesTruncatedCountDoesNotLoop(t *testing.T) {
	app := chapterPagesFixture(t, "abcdef\n")
	app.document.Size += 20
	app.document.Chapters[0].End += 20
	if app.openChapter(0, 0) || app.message == "" || app.chapterPagination != nil || app.dirty {
		t.Fatal("short source read committed an incomplete page count")
	}
}
