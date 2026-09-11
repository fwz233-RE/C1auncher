package main

import (
	"fmt"
	"sort"
)

// chapterPages records the canonical layout obtained by reading from the chapter
// start through successive bounded Paginate windows. Only byte boundaries are
// retained; text from other windows is discarded. Saved positions remain source
// offsets, including positions inside a canonical page from an older layout.
type chapterPages struct {
	document *Document
	chapter  Chapter
	face     TextFace
	starts   []int64
	windows  []chapterPageWindow
}

type chapterPageWindow struct {
	start, end int64
	firstPage  int
}

func countChapterPages(document *Document, chapter Chapter, face TextFace) (*chapterPages, error) {
	index := &chapterPages{document: document, chapter: chapter, face: face}
	for start := chapter.Start; ; {
		pages, end, err := Paginate(document, chapter, start, face, readerTextWidth, readerBodyHeight)
		if err != nil {
			return nil, err
		}
		if end < chapter.End && end <= start {
			return nil, fmt.Errorf("cannot count chapter pages: read stopped at %d", start)
		}
		index.windows = append(index.windows, chapterPageWindow{start: start, end: end, firstPage: len(index.starts)})
		for _, page := range pages {
			index.starts = append(index.starts, page.Start)
		}
		if end >= chapter.End {
			break
		}
		start = end
	}
	return index, nil
}

func (index *chapterPages) pageAt(offset int64) int {
	page := sort.Search(len(index.starts), func(i int) bool { return index.starts[i] > offset }) - 1
	if page < 0 {
		page = 0 // The omitted chapter heading belongs to page one.
	}
	return page
}

func (app *readerApp) chapterPagesMatch() bool {
	index := app.chapterPagination
	return index != nil && app.document != nil && app.chapterIndex >= 0 && app.chapterIndex < len(app.document.Chapters) &&
		index.document == app.document && index.chapter == app.document.Chapters[app.chapterIndex] && index.face == app.bodyFace
}

// chapterPageLabel is deliberately read-only and performs no filesystem I/O.
// A missing index is not a licence to mistake a partial window for the chapter.
func (app *readerApp) chapterPageLabel() string {
	bookmark, ok := app.currentBookmark()
	if !ok || !app.chapterPagesMatch() || len(app.chapterPagination.starts) == 0 {
		return "页 —/—"
	}
	index := app.chapterPagination
	return fmt.Sprintf("页 %d/%d", index.pageAt(bookmark.Offset)+1, len(index.starts))
}

func (app *readerApp) countedChapter(chapterIndex int) (*chapterPages, error) {
	chapter := app.document.Chapters[chapterIndex]
	index := app.chapterPagination
	if index != nil && index.document == app.document && index.chapter == chapter && index.face == app.bodyFace {
		return index, nil
	}
	return countChapterPages(app.document, chapter, app.bodyFace)
}

// readWindow restores exactly the canonical window, rather than repaginating
// the rest of the chapter from an arbitrary resume offset. Only the first page
// is shortened to preserve that byte offset; subsequent pages keep their normal
// boundaries, so forward/backward navigation advances one chapter page at a time.
func (index *chapterPages) readWindow(offset int64) ([]Page, int64, error) {
	page := index.pageAt(offset)
	windowIndex := sort.Search(len(index.windows), func(i int) bool { return index.windows[i].firstPage > page }) - 1
	window := index.windows[windowIndex]
	chapter := index.chapter
	chapter.End = window.end
	pages, end, err := Paginate(index.document, chapter, window.start, index.face, readerTextWidth, readerBodyHeight)
	if err != nil {
		return nil, offset, err
	}
	if end != window.end {
		return nil, offset, fmt.Errorf("cannot load chapter pages: expected end %d, got %d", window.end, end)
	}
	first := page - window.firstPage
	if first >= len(pages) || pages[first].Start != index.starts[page] {
		return nil, offset, fmt.Errorf("chapter page layout changed at %d", offset)
	}
	pages = pages[first:]
	if offset > pages[0].Start {
		partial := chapter
		partial.End = pages[0].End
		resumed, resumedEnd, err := Paginate(index.document, partial, offset, index.face, readerTextWidth, readerBodyHeight)
		if err != nil {
			return nil, offset, err
		}
		if resumedEnd != partial.End || len(resumed) != 1 || resumed[0].Start != offset {
			return nil, offset, fmt.Errorf("cannot restore chapter page at %d", offset)
		}
		pages[0] = resumed[0]
	}
	return pages, end, nil
}

// All counting and reads happen before the reading state or its active cache
// changes. This also makes transitions to a previous chapter's last page atomic.
func (app *readerApp) openCountedChapter(chapterIndex int, offset int64, last bool) bool {
	index, err := app.countedChapter(chapterIndex)
	if err != nil {
		app.message = err.Error()
		return false
	}
	if last {
		offset = index.starts[len(index.starts)-1]
	}
	pages, end, err := index.readWindow(offset)
	if err != nil {
		app.message = err.Error()
		return false
	}
	app.chapterIndex = chapterIndex
	app.revealChapter(chapterIndex)
	app.chapterPagination = index
	app.pages, app.pageIndex = pages, 0
	app.windowStart, app.windowEnd = offset, end
	app.view = viewReader
	app.dirty = true
	return true
}

func (app *readerApp) previousChapterPage() {
	if app.document == nil || app.chapterIndex < 0 || app.chapterIndex >= len(app.document.Chapters) {
		return
	}
	bookmark, ok := app.currentBookmark()
	if !ok {
		return
	}
	index, err := app.countedChapter(app.chapterIndex)
	if err != nil {
		app.message = err.Error()
		return
	}
	if page := index.pageAt(bookmark.Offset); page > 0 {
		app.openChapter(app.chapterIndex, index.starts[page-1])
		return
	}
	if previous, ok := app.document.readableChapter(app.chapterIndex-1, -1); ok {
		app.openCountedChapter(previous, app.document.Chapters[previous].Start, true)
	}
}
