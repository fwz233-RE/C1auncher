package main

import (
	"fmt"
	"image"

	"c1device"
)

type viewMode uint8

const (
	viewShelf viewMode = iota
	viewChapters
	viewBookmarks
	viewReader
	viewPercentJump
)

const (
	readerUIFontSize    = 15
	readerBodyFontSize  = 17
	readerBodyThreshold = 112
	readerHeaderBottom  = 24
	readerBodyTop       = 26
	readerBodyBottom    = 128
	readerFooterTop     = 132
	readerTextWidth     = c1device.DisplayWidth - 14
	readerBodyHeight    = readerBodyBottom - readerBodyTop
	readerListRowHeight = 22
	readerListFooterTop = 130
	chapterListHint     = "↑↓选择  ←返回  →阅读  P书签"
	bookmarkListHint    = "↑↓选择  ←返回  →跳转  P删除"
	readerFooterPrefix  = "←章节  ↑上页  ↓下页  "
)

type readerApp struct {
	books           []Book
	bookIndex       int
	chapterIndex    int
	chapterPick     int
	bookmarkPick    int
	view            viewMode
	readerOrigin    viewMode
	document        *Document
	pages           []Page
	pageIndex       int
	windowStart     int64
	windowEnd       int64
	resumeOffset    int64
	uiFace          *c1device.Face
	bodyFace        *c1device.Face
	store           ProgressStore
	bookmarkStore   BookmarkStore
	bookmarks       []Bookmark
	dirty           bool
	message         string
	percentInput    string
	percentValue    int
	percentOrigin   viewMode
	expandedVolumes map[int]bool
	volumeBodyPick  bool
}

func newReaderApp(booksDir string, uiFace, bodyFace *c1device.Face, store ProgressStore, bookmarkStore BookmarkStore) (*readerApp, error) {
	books, err := ScanLibrary(booksDir)
	if err != nil {
		return nil, err
	}
	return &readerApp{
		books: books, uiFace: uiFace, bodyFace: bodyFace,
		store: store, bookmarkStore: bookmarkStore, view: viewShelf,
	}, nil
}

func (app *readerApp) handle(key c1device.Key) (exit bool) {
	return app.handleEvent(c1device.Event{Key: key})
}

func (app *readerApp) handleEvent(event c1device.Event) (exit bool) {
	// Only navigation repeats. Holding an action key must not toggle a
	// bookmark twice or carry an open/back action into the next view.
	if event.Repeat {
		switch event.Key {
		case c1device.KeyUp, c1device.KeyDown, c1device.KeyVolumeUp, c1device.KeyVolumeDown:
		default:
			return false
		}
	}
	app.message = ""
	switch app.view {
	case viewShelf:
		switch event.Key {
		case c1device.KeyUp:
			app.bookIndex = moveSelection(app.bookIndex, -1, len(app.books))
		case c1device.KeyDown:
			app.bookIndex = moveSelection(app.bookIndex, 1, len(app.books))
		case c1device.KeyOK, c1device.KeyRight:
			app.openSelectedBook()
		case c1device.KeyBack:
			return true
		}
	case viewChapters:
		switch event.Key {
		case c1device.KeyUp:
			app.moveDirectorySelection(-chapterSelectionDelta(event))
		case c1device.KeyDown:
			app.moveDirectorySelection(chapterSelectionDelta(event))
		case c1device.KeyOK, c1device.KeyRight:
			app.activateDirectorySelection()
		case c1device.KeyRune:
			if event.Rune == 'o' || event.Rune == 'O' {
				app.openPercentJump()
			}
		case c1device.KeyPause:
			app.bookmarkPick = 0
			app.view = viewBookmarks
		case c1device.KeyBack, c1device.KeyLeft:
			app.leaveDirectory()
		}
	case viewBookmarks:
		switch event.Key {
		case c1device.KeyUp:
			app.bookmarkPick = moveSelection(app.bookmarkPick, -1, len(app.bookmarks))
		case c1device.KeyDown:
			app.bookmarkPick = moveSelection(app.bookmarkPick, 1, len(app.bookmarks))
		case c1device.KeyOK, c1device.KeyRight:
			app.readerOrigin = viewBookmarks
			app.openSelectedBookmark()
		case c1device.KeyPause:
			app.deleteSelectedBookmark()
		case c1device.KeyBack, c1device.KeyLeft:
			app.view = viewChapters
		}
	case viewPercentJump:
		app.handlePercentJump(event)
	case viewReader:
		switch event.Key {
		case c1device.KeyLeft, c1device.KeyBack:
			if bookmark, ok := app.currentBookmark(); ok {
				app.resumeOffset = bookmark.Offset
			}
			app.percentInput = ""
			app.revealChapter(app.chapterIndex)
			if app.readerOrigin == viewBookmarks {
				app.view = viewBookmarks
			} else {
				app.view = viewChapters
			}
		case c1device.KeyUp, c1device.KeyVolumeUp:
			app.previousPage()
		case c1device.KeyDown, c1device.KeyVolumeDown:
			app.nextPage()
		case c1device.KeyRight:
			app.toggleBookmark()
		case c1device.KeyRune:
			if event.Rune == 'o' || event.Rune == 'O' {
				app.openPercentJump()
			}
		}
	}
	return false
}

func chapterSelectionDelta(event c1device.Event) int {
	if event.Repeat {
		return 5
	}
	return 1
}

func moveSelection(current, delta, count int) int {
	if count == 0 {
		return 0
	}
	current += delta
	if current < 0 {
		return 0
	}
	if current >= count {
		return count - 1
	}
	return current
}

func (app *readerApp) openSelectedBook() {
	if len(app.books) == 0 {
		return
	}
	// The delayed save belongs to the old document. Flush it before replacing
	// that document, including when reopening the same book from the shelf.
	if err := app.saveProgress(); err != nil {
		app.message = err.Error()
		return
	}
	document, err := OpenDocument(app.books[app.bookIndex].Path)
	if err != nil {
		app.message = err.Error()
		return
	}
	app.document = document
	app.expandedVolumes = nil
	app.volumeBodyPick = false
	app.chapterIndex = 0
	app.chapterPick = 0
	if rows, _ := app.directoryRows(); len(rows) > 0 {
		app.chapterPick = rows[0].chapter
	}
	app.bookmarkPick = 0
	app.pages = nil
	app.pageIndex = 0
	app.resumeOffset = 0
	app.bookmarks = nil
	if saved, ok, err := app.store.Load(document.Path); err == nil && ok {
		if validPosition(document, saved.Chapter, saved.Offset) {
			app.revealChapter(saved.Chapter)
			app.chapterIndex = saved.Chapter
			app.resumeOffset = saved.Offset
		}
	}
	bookmarks, err := app.bookmarkStore.Load(document.Path)
	if err != nil {
		app.message = err.Error()
	} else {
		app.bookmarks = validBookmarks(document, bookmarks)
	}
	app.view = viewChapters
}

func validPosition(document *Document, chapterIndex int, offset int64) bool {
	if document == nil || chapterIndex < 0 || chapterIndex >= len(document.Chapters) {
		return false
	}
	chapter := document.Chapters[chapterIndex]
	return offset >= chapter.Start && offset < chapter.End
}

func validBookmarks(document *Document, bookmarks []Bookmark) []Bookmark {
	valid := make([]Bookmark, 0, len(bookmarks))
	for _, bookmark := range bookmarks {
		if validPosition(document, bookmark.Chapter, bookmark.Offset) {
			valid = append(valid, bookmark)
		}
	}
	return valid
}

func (app *readerApp) openChapter(chapterIndex int, offset int64) bool {
	if app.document == nil || chapterIndex < 0 || chapterIndex >= len(app.document.Chapters) {
		return false
	}
	originalChapter := chapterIndex
	var ok bool
	chapterIndex, offset, ok = app.document.readingTarget(chapterIndex, offset)
	if !ok {
		app.message = "此书只有卷标题，暂无正文"
		return false
	}
	if chapterIndex != originalChapter {
		var err error
		offset, err = app.document.AlignLineStart(offset, app.document.Chapters[chapterIndex].Start)
		if err != nil {
			app.message = err.Error()
			return false
		}
	}
	if !validPosition(app.document, chapterIndex, offset) {
		offset = app.document.Chapters[chapterIndex].Start
	}
	chapter := app.document.Chapters[chapterIndex]
	pages, end, err := Paginate(app.document, chapter, offset, app.bodyFace, readerTextWidth, readerBodyHeight)
	if err != nil {
		app.message = err.Error()
		return false
	}
	// Commit chapter and pages together only after the read succeeds.
	app.chapterIndex = chapterIndex
	app.revealChapter(chapterIndex)
	app.pages, app.pageIndex = pages, 0
	app.windowStart, app.windowEnd = offset, end
	app.view = viewReader
	app.dirty = true
	return true
}

func (app *readerApp) loadChapter(offset int64) {
	app.openChapter(app.chapterIndex, offset)
}

func (app *readerApp) nextPage() {
	if app.pageIndex+1 < len(app.pages) {
		app.pageIndex++
		app.dirty = true
		return
	}
	chapter := app.document.Chapters[app.chapterIndex]
	if app.windowEnd < chapter.End {
		app.loadChapter(app.windowEnd)
		return
	}
	if next, ok := app.document.readableChapter(app.chapterIndex+1, 1); ok {
		app.openChapter(next, app.document.Chapters[next].Start)
	}
}

func (app *readerApp) previousPage() {
	if app.pageIndex > 0 {
		app.pageIndex--
		app.dirty = true
		return
	}
	chapter := app.document.Chapters[app.chapterIndex]
	if app.windowStart > chapter.Start {
		start := app.windowStart - maxChapterWindow
		if start < chapter.Start {
			start = chapter.Start
		}
		if aligned, alignErr := app.document.AlignLineStart(start, chapter.Start); alignErr == nil {
			start = aligned
		}
		pages, end, err := Paginate(app.document, chapter, start, app.bodyFace, readerTextWidth, readerBodyHeight)
		if err == nil && len(pages) > 0 {
			app.pages, app.pageIndex = pages, len(pages)-1
			app.windowStart, app.windowEnd = start, end
			app.dirty = true
		}
		return
	}
	if previousIndex, ok := app.document.readableChapter(app.chapterIndex-1, -1); ok {
		previous := app.document.Chapters[previousIndex]
		start := previous.Start
		if previous.End-previous.Start > maxChapterWindow {
			start = previous.End - maxChapterWindow
			if aligned, alignErr := app.document.AlignLineStart(start, previous.Start); alignErr == nil {
				start = aligned
			}
		}
		if app.openChapter(previousIndex, start) && len(app.pages) > 0 {
			app.pageIndex = len(app.pages) - 1
		}
	}
}

func (app *readerApp) jumpToPercent(percent int) bool {
	if percent < 0 || percent > 100 {
		return false
	}
	return app.jumpToPercentUnits(percent * percentScale)
}

func (app *readerApp) jumpToPercentUnits(units int) bool {
	chapterIndex, offset, ok := positionForPercentUnits(app.document, units)
	if !ok {
		app.message = "无法跳转：没有可读正文"
		return false
	}
	aligned, err := app.document.AlignLineStart(offset, app.document.Chapters[chapterIndex].Start)
	if err != nil {
		app.message = err.Error()
		return false
	}
	return app.openChapter(chapterIndex, aligned)
}

func positionForPercent(document *Document, percent int) (int, int64, bool) {
	if percent < 0 || percent > 100 {
		return 0, 0, false
	}
	return positionForPercentUnits(document, percent*percentScale)
}

func positionForPercentUnits(document *Document, units int) (int, int64, bool) {
	if document == nil || document.Size <= 0 || units < 0 || units > maxPercentUnits || len(document.Chapters) == 0 {
		return 0, 0, false
	}
	// Quotient/remainder avoids overflowing int64 for very large documents.
	target := document.Size/maxPercentUnits*int64(units) + document.Size%maxPercentUnits*int64(units)/maxPercentUnits
	if target >= document.Size {
		target = document.Size - 1
	}
	if target < document.Chapters[0].Start {
		target = document.Chapters[0].Start
	}
	for index, chapter := range document.Chapters {
		if target >= chapter.Start && target < chapter.End {
			return document.readingTarget(index, target)
		}
	}
	return 0, 0, false
}

func (app *readerApp) currentBookmark() (Bookmark, bool) {
	if app.document == nil || len(app.pages) == 0 || app.pageIndex < 0 || app.pageIndex >= len(app.pages) {
		return Bookmark{}, false
	}
	return Bookmark{Chapter: app.chapterIndex, Offset: app.pages[app.pageIndex].Start}, true
}

func (app *readerApp) readerHint() string {
	action := "→书签"
	if bookmark, ok := app.currentBookmark(); ok {
		for _, saved := range app.bookmarks {
			if saved == bookmark {
				action = "→删除"
				break
			}
		}
	}
	prefix := readerFooterPrefix
	if app.readerOrigin == viewBookmarks {
		prefix = "←书签  ↑上页  ↓下页  "
	}
	return prefix + action
}

func (app *readerApp) toggleBookmark() {
	bookmark, ok := app.currentBookmark()
	if !ok {
		return
	}
	for index, saved := range app.bookmarks {
		if saved == bookmark {
			app.bookmarks = append(app.bookmarks[:index], app.bookmarks[index+1:]...)
			app.saveBookmarks()
			return
		}
	}
	app.bookmarks = normalizeBookmarks(append(app.bookmarks, bookmark))
	app.saveBookmarks()
}

func (app *readerApp) openSelectedBookmark() {
	if app.bookmarkPick < 0 || app.bookmarkPick >= len(app.bookmarks) {
		return
	}
	bookmark := app.bookmarks[app.bookmarkPick]
	app.openChapter(bookmark.Chapter, bookmark.Offset)
}

func (app *readerApp) deleteSelectedBookmark() {
	if app.bookmarkPick < 0 || app.bookmarkPick >= len(app.bookmarks) {
		return
	}
	app.bookmarks = append(app.bookmarks[:app.bookmarkPick], app.bookmarks[app.bookmarkPick+1:]...)
	app.bookmarkPick = moveSelection(app.bookmarkPick, 0, len(app.bookmarks))
	app.saveBookmarks()
}

func (app *readerApp) saveBookmarks() {
	if err := app.bookmarkStore.Save(app.document.Path, app.bookmarks); err != nil {
		app.message = err.Error()
	}
}

func (app *readerApp) saveProgress() error {
	if !app.dirty {
		return nil
	}
	progress, ok := app.currentProgress()
	if !ok {
		return fmt.Errorf("cannot read current book progress")
	}
	if err := app.store.Save(progress); err != nil {
		return err
	}
	app.dirty = false
	return nil
}

func (app *readerApp) currentProgress() (Progress, bool) {
	if app.document == nil || app.chapterIndex < 0 || app.chapterIndex >= len(app.document.Chapters) {
		return Progress{}, false
	}
	fingerprint, err := fingerprint(app.document.Path)
	if err != nil {
		return Progress{}, false
	}
	offset := app.document.Chapters[app.chapterIndex].Start
	if len(app.pages) > 0 && app.pageIndex >= 0 && app.pageIndex < len(app.pages) {
		offset = app.pages[app.pageIndex].Start
	} else if validPosition(app.document, app.chapterIndex, app.resumeOffset) {
		offset = app.resumeOffset
	}
	return Progress{Fingerprint: fingerprint, Path: app.document.Path, Chapter: app.chapterIndex, Offset: offset, LayoutVersion: layoutVersion}, true
}

func (app *readerApp) render() c1device.Frame {
	canvas := c1device.NewCanvas()
	canvas.Clear()
	switch app.view {
	case viewShelf:
		app.renderList(canvas, "书架", bookNames(app.books), app.bookIndex, "本", "↑↓选择  →打开  BACK退出")
	case viewChapters:
		app.renderDirectory(canvas)
	case viewBookmarks:
		app.renderList(canvas, "书签", app.bookmarkLabels(), app.bookmarkPick, "个", bookmarkListHint)
	case viewReader:
		app.renderReader(canvas)
	case viewPercentJump:
		app.renderPercentJump(canvas)
	}
	return canvas.Frame(128)
}

func bookNames(books []Book) []string {
	names := make([]string, len(books))
	for index := range books {
		names[index] = books[index].Name
	}
	return names
}

func (app *readerApp) bookmarkLabels() []string {
	labels := make([]string, 0, len(app.bookmarks))
	for _, bookmark := range app.bookmarks {
		if !validPosition(app.document, bookmark.Chapter, bookmark.Offset) {
			continue
		}
		percent := float64(bookmark.Offset) * 100 / float64(app.document.Size)
		labels = append(labels, fmt.Sprintf("%s  %.1f%%", app.document.contextualChapterTitle(bookmark.Chapter), percent))
	}
	return labels
}

func (app *readerApp) renderList(canvas *c1device.Canvas, title string, items []string, selected int, unit, hint string) {
	const rowTop = readerHeaderBottom + 2
	count := fmt.Sprintf("%d %s", len(items), unit)
	titleWidth := 210
	if app.view == viewChapters {
		count = app.directoryCount()
		buttonLeft := 290 - app.uiFace.Measure(count) - 8 - 77
		titleWidth = buttonLeft - 12
		canvas.DrawTextInverted(app.uiFace, image.Rect(buttonLeft, 1, buttonLeft+77, 22), buttonLeft+5, 1, "O跳转")
	}
	canvas.DrawText(app.uiFace, 6, 1, fitText(app.uiFace, title, titleWidth))
	canvas.DrawTextRight(app.uiFace, 290, 1, count)
	canvas.FillRect(image.Rect(4, readerHeaderBottom-2, 292, readerHeaderBottom))
	if len(items) == 0 {
		emptyTitle, emptyHint := "暂无内容", "请连接设备后添加文件"
		if title == "书签" {
			emptyTitle, emptyHint = "暂无书签", "阅读时按 → 添加书签"
		}
		canvas.DrawTextCentered(app.uiFace, c1device.DisplayWidth/2, 52, emptyTitle)
		canvas.DrawTextCentered(app.uiFace, c1device.DisplayWidth/2, 78, emptyHint)
	} else {
		visible := (readerListFooterTop - rowTop) / readerListRowHeight
		start := selected - visible/2
		if start < 0 {
			start = 0
		}
		if start+visible > len(items) {
			start = len(items) - visible
			if start < 0 {
				start = 0
			}
		}
		for index := start; index < len(items) && index < start+visible; index++ {
			top := rowTop + (index-start)*readerListRowHeight
			prefix := "  "
			if index == selected {
				prefix = "› "
			}
			text := fitText(app.uiFace, prefix+items[index], 274)
			if index == selected {
				canvas.DrawTextInverted(app.uiFace, image.Rect(5, top, 286, top+readerListRowHeight-1), 8, top+1, text)
			} else {
				canvas.DrawText(app.uiFace, 8, top+1, text)
			}
		}
		canvas.DrawScrollIndicator(290, rowTop, readerListFooterTop, selected, len(items))
	}
	footer := hint
	if app.message != "" {
		footer = fitText(app.uiFace, app.message, 280)
	}
	canvas.DrawInvertedTextBar(app.uiFace, image.Rect(0, readerListFooterTop, c1device.DisplayWidth, c1device.DisplayHeight), footer)
}

func (app *readerApp) renderReader(canvas *c1device.Canvas) {
	percent := 0.0
	if len(app.pages) > 0 && app.document.Size > 0 {
		percent = float64(app.pages[app.pageIndex].Start) * 100 / float64(app.document.Size)
	}
	canvas.DrawText(app.uiFace, 6, 1, fitText(app.uiFace, app.document.contextualChapterTitle(app.chapterIndex), 216))
	canvas.DrawTextRight(app.uiFace, 290, 1, fmt.Sprintf("%.2f%%", percent))
	canvas.FillRect(image.Rect(4, readerHeaderBottom-2, 292, readerHeaderBottom))
	if len(app.pages) > 0 {
		page := app.pages[app.pageIndex]
		y := readerBodyTop
		for _, line := range page.Lines {
			canvas.DrawTextThreshold(app.bodyFace, 7, y, line, readerBodyThreshold)
			y += app.bodyFace.LineHeight()
		}
	}
	footer := app.readerHint()
	if app.message != "" {
		footer = app.message
	}
	canvas.DrawInvertedTextBar(app.uiFace, image.Rect(0, readerFooterTop, c1device.DisplayWidth, c1device.DisplayHeight), footer)
}

func fitText(face *c1device.Face, text string, width int) string {
	if face.Measure(text) <= width {
		return text
	}
	runes := []rune(text)
	for len(runes) > 0 && face.Measure(string(runes)+"…") > width {
		runes = runes[:len(runes)-1]
	}
	return string(runes) + "…"
}
