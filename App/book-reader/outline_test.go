package main

import (
	"bytes"
	"encoding/json"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"c1device"
	"golang.org/x/text/encoding/simplifiedchinese"
)

const volumeBook = "卷前说明。\n第一卷 开端\n卷首说明，必须保留。\n第一章 初遇\n甲乙丙丁。\n第二章 继续\n这是第二章。\n第二卷 新程\n第一章 初遇\n这是新卷第一章。\n第二章 结尾\n末章第一行。\n末章最后一行。\n第三卷 待续\n"

func volumeFixture(t *testing.T) *readerApp {
	t.Helper()
	app := readingFixture(t)
	if err := os.WriteFile(app.document.Path, []byte(volumeBook), 0644); err != nil {
		t.Fatal(err)
	}
	document, err := OpenDocument(app.document.Path)
	if err != nil {
		t.Fatal(err)
	}
	app.document, app.pages = document, nil
	app.books = []Book{{Path: document.Path, Name: "卷章测试.txt"}}
	app.view, app.dirty = viewChapters, false
	return app
}

func TestVolumeClassificationUsesHeadingUnitNotTitleWords(t *testing.T) {
	for title, want := range map[string]bool{
		"第一卷 开端": true, "卷一：开端": true, "第2部 远行": true,
		"篇三 新程": true, "第一篇 新程": true,
		"第一章 新卷": false, "第二章 新篇": false, "第一节：开端": false,
		"第一回 书卷": false, "正文": false,
	} {
		if got := isVolumeTitle(title); got != want {
			t.Errorf("isVolumeTitle(%q)=%v want=%v", title, got, want)
		}
	}
}

func TestVolumeOutlinePreservesRawOffsetsAcrossEncodingsAndCache(t *testing.T) {
	for _, encoding := range []string{"utf8", "bom-crlf", "gb18030"} {
		t.Run(encoding, func(t *testing.T) {
			raw := []byte(volumeBook)
			var err error
			if encoding == "bom-crlf" {
				raw = append([]byte{0xef, 0xbb, 0xbf}, []byte(strings.ReplaceAll(volumeBook, "\n", "\r\n"))...)
			} else if encoding == "gb18030" {
				raw, err = simplifiedchinese.GB18030.NewEncoder().Bytes(raw)
				if err != nil {
					t.Fatal(err)
				}
			}
			path := filepath.Join(t.TempDir(), "volumes.txt")
			if err := os.WriteFile(path, raw, 0644); err != nil {
				t.Fatal(err)
			}
			doc, err := OpenDocument(path)
			if err != nil {
				t.Fatal(err)
			}
			if len(doc.Chapters) != 8 {
				t.Fatalf("raw sections=%d", len(doc.Chapters))
			}
			outline := doc.chapterOutline()
			if outline.volumes != 3 || outline.chapters != 4 {
				t.Fatalf("outline=%+v", outline)
			}
			for _, index := range []int{1, 4, 7} {
				if !outline.entries[index].volume {
					t.Fatalf("section %d is not volume", index)
				}
			}
			if !doc.Chapters[1].HasBody || doc.Chapters[4].HasBody || doc.Chapters[7].HasBody {
				t.Fatal("volume introduction metadata is wrong")
			}
			if outline.entries[2].parent != 1 || outline.entries[5].parent != 4 || outline.entries[0].parent != -1 {
				t.Fatal("volume ownership is wrong")
			}
			var reconstructed []byte
			for _, chapter := range doc.Chapters {
				reconstructed = append(reconstructed, raw[chapter.Start:chapter.End]...)
			}
			if !bytes.Equal(reconstructed, raw[doc.Chapters[0].Start:]) {
				t.Fatal("volume grouping lost source bytes")
			}
			cached, err := OpenDocument(path)
			if err != nil {
				t.Fatal(err)
			}
			if !reflect.DeepEqual(doc.Chapters, cached.Chapters) || !reflect.DeepEqual(outline, cached.chapterOutline()) {
				t.Fatal("cold and cached outlines differ")
			}
		})
	}
}

func TestVolumeDirectoryExpandCollapseAndReadIntroduction(t *testing.T) {
	app := volumeFixture(t)
	rows, _ := app.directoryRows()
	if len(rows) != 4 || app.directoryCount() != "3卷/4章" {
		t.Fatalf("collapsed rows=%+v", rows)
	}
	app.handle(c1device.KeyDown)
	if app.chapterPick != 1 || app.directoryHint() != "←返回  ↑上移  ↓下移  →展开" {
		t.Fatal("volume header not selected")
	}
	app.handle(c1device.KeyRight)
	rows, _ = app.directoryRows()
	if len(rows) != 7 || app.view != viewChapters || app.dirty {
		t.Fatal("expand opened or saved content")
	}
	app.handle(c1device.KeyDown)
	if app.chapterPick != 1 || !app.volumeBodyPick {
		t.Fatal("missing introduction row")
	}
	app.handle(c1device.KeyRight)
	if app.view != viewReader || app.chapterIndex != 1 || !strings.Contains(strings.Join(app.pages[0].Lines, ""), "卷首说明") {
		t.Fatal("volume introduction is unreadable")
	}
	app.handle(c1device.KeyLeft)
	if app.view != viewChapters || !app.volumeBodyPick {
		t.Fatal("intro return target changed")
	}
	app.handle(c1device.KeyLeft)
	if app.view != viewChapters || app.expandedVolumes[1] || app.volumeBodyPick || app.chapterPick != 1 {
		t.Fatal("left did not collapse volume")
	}
	app.handle(c1device.KeyLeft)
	if app.view != viewShelf {
		t.Fatal("left from collapsed volume did not return to shelf")
	}
}

func TestVolumeDirectoryLongPressMovesVisibleRows(t *testing.T) {
	app := volumeFixture(t)
	app.chapterPick = 1
	app.handle(c1device.KeyRight)
	app.handleEvent(c1device.Event{Key: c1device.KeyDown, Repeat: true})
	if app.chapterPick != 7 {
		t.Fatalf("long press selected raw index %d, want last visible row 7", app.chapterPick)
	}
	app.handle(c1device.KeyUp)
	if app.chapterPick != 4 {
		t.Fatal("collapsed child leaked into navigation")
	}
	app.handle(c1device.KeyRight)
	app.handle(c1device.KeyDown)
	if app.chapterPick != 5 || app.volumeBodyPick {
		t.Fatal("heading-only volume has a fake body row")
	}
	app.handle(c1device.KeyBack)
	if app.chapterPick != 4 || app.expandedVolumes[4] {
		t.Fatal("BACK did not collapse selected chapter's volume")
	}
}

func TestSequentialReadingSkipsEmptyVolumesBothWays(t *testing.T) {
	app := volumeFixture(t)
	if !app.openChapter(3, 0) {
		t.Fatal(app.message)
	}
	app.pageIndex = len(app.pages) - 1
	app.nextPage()
	if app.chapterIndex != 5 {
		t.Fatalf("next chapter=%d want=5", app.chapterIndex)
	}
	app.previousPage()
	if app.chapterIndex != 3 {
		t.Fatalf("previous chapter=%d want=3", app.chapterIndex)
	}
	if !app.openChapter(2, 0) {
		t.Fatal(app.message)
	}
	app.previousPage()
	if app.chapterIndex != 1 {
		t.Fatal("backward skipped readable introduction")
	}
	if !app.openChapter(6, 0) {
		t.Fatal(app.message)
	}
	app.pageIndex = len(app.pages) - 1
	app.nextPage()
	if app.chapterIndex != 6 {
		t.Fatal("trailing empty volume produced a blank page")
	}
}

func TestVolumePercentJumpAndCancelRevealCorrectParent(t *testing.T) {
	app := volumeFixture(t)
	app.chapterPick = 4
	app.handleEvent(keyO())
	app.handle(c1device.KeyUp)
	app.handle(c1device.KeyLeft)
	if app.view != viewChapters || app.chapterPick != 4 || app.expandedVolumes[4] || app.dirty {
		t.Fatal("cancel changed collapsed directory")
	}
	app.handleEvent(keyO())
	app.percentValue = 10000
	app.handle(c1device.KeyOK)
	if app.view != viewReader || app.chapterIndex != 6 || !app.expandedVolumes[4] {
		t.Fatal("jump did not reveal last readable chapter")
	}
	if !strings.Contains(strings.Join(app.pages[0].Lines, ""), "末章最后一行") {
		t.Fatal("100% jump did not reach book end")
	}
	if label := app.document.positionLabel(6); label != "第二卷  第 4 / 4 章" {
		t.Fatalf("preview label=%q", label)
	}
	app.handle(c1device.KeyLeft)
	rows, selected := app.directoryRows()
	if rows[selected].chapter != 6 || app.view != viewChapters {
		t.Fatal("jump return selected wrong visible row")
	}
}

func TestLegacyCacheProgressAndBookmarksKeepStableChapterIDs(t *testing.T) {
	app := volumeFixture(t)
	doc := app.document
	original := append([]Chapter(nil), doc.Chapters...)
	mark, err := fingerprint(doc.Path)
	if err != nil {
		t.Fatal(err)
	}
	legacy := append([]Chapter(nil), original...)
	for i := range legacy {
		legacy[i].HasBody = false
	}
	if err := saveChapterIndexCache(doc.Path, chapterIndexCache{Version: 1, Path: doc.Path, Fingerprint: mark, Encoding: doc.Encoding, Chapters: legacy}); err != nil {
		t.Fatal(err)
	}
	offset := doc.Chapters[5].Start + int64(len("第一章 初遇\n"))
	if err := app.store.Save(Progress{Path: doc.Path, Fingerprint: mark, Chapter: 5, Offset: offset}); err != nil {
		t.Fatal(err)
	}
	oldBookmarks := []Bookmark{{Chapter: 1, Offset: doc.Chapters[1].Start}, {Chapter: 5, Offset: offset}}
	if err := app.bookmarkStore.Save(doc.Path, oldBookmarks); err != nil {
		t.Fatal(err)
	}
	app.openSelectedBook()
	if app.chapterIndex != 5 || app.resumeOffset != offset || !app.expandedVolumes[4] {
		t.Fatal("old progress did not restore into volume")
	}
	if !reflect.DeepEqual(app.document.Chapters, original) || !reflect.DeepEqual(app.bookmarks, oldBookmarks) {
		t.Fatal("cache rebuild renumbered chapters or lost bookmarks")
	}
	data, err := os.ReadFile(chapterIndexCachePath(doc.Path))
	if err != nil {
		t.Fatal(err)
	}
	var rebuilt chapterIndexCache
	if err := json.Unmarshal(data, &rebuilt); err != nil {
		t.Fatal(err)
	}
	if rebuilt.Version != chapterIndexCacheVersion || !rebuilt.Chapters[1].HasBody {
		t.Fatal("legacy cache not rebuilt")
	}
	app.handle(c1device.KeyRight)
	if app.pages[app.pageIndex].Start != offset {
		t.Fatal("old reading offset changed")
	}
	app.handle(c1device.KeyLeft)
	app.handle(c1device.KeyPause)
	app.bookmarkPick = 1
	app.handle(c1device.KeyRight)
	if !strings.HasPrefix(app.readerHint(), "←书签") || app.chapterIndex != 5 {
		t.Fatal("bookmark navigation lost origin or chapter ID")
	}
	if !strings.HasPrefix(app.bookmarkLabels()[1], "第二卷 · 第一章") {
		t.Fatal("bookmark does not disambiguate repeated chapter names")
	}
	app.handle(c1device.KeyLeft)
	if app.view != viewBookmarks {
		t.Fatal("bookmark reader did not return to bookmarks")
	}
}

func TestBooksWithOnlyVolumeHeadingsOrOnlyVolumeBody(t *testing.T) {
	for _, body := range []string{"", "卷内正文。\n"} {
		app := readingFixture(t)
		text := "第一卷 开篇\n" + body
		if err := os.WriteFile(app.document.Path, []byte(text), 0644); err != nil {
			t.Fatal(err)
		}
		var err error
		app.document, err = OpenDocument(app.document.Path)
		if err != nil {
			t.Fatal(err)
		}
		app.pages, app.view, app.dirty = nil, viewChapters, false
		app.handle(c1device.KeyRight)
		rows, _ := app.directoryRows()
		if body == "" {
			app.handleEvent(keyO())
			if app.view != viewChapters || app.message == "" {
				t.Fatal("heading-only book opened an unusable jump screen")
			}
			if len(rows) != 1 || app.jumpToPercent(0) || app.dirty {
				t.Fatal("heading-only book created readable content")
			}
		} else {
			if len(rows) != 2 || !app.jumpToPercent(0) || len(app.pages[0].Lines) == 0 {
				t.Fatal("volume-only book lost its body")
			}
		}
	}
}

func TestRepeatedVolumeHeadingsMergeWithoutLosingContinuationBody(t *testing.T) {
	app := readingFixture(t)
	text := "第一卷：开端\n第一章 初遇\n正文一。\n第一卷 ：开端\n补充说明。\n第二章 继续\n正文二。\n第一卷:开端\n第三章 后续\n正文三。\n第二卷：新程\n第一章 起步\n正文四。\n第三卷：重返\n第一章 重返\n正文五。\n"
	if err := os.WriteFile(app.document.Path, []byte(text), 0644); err != nil {
		t.Fatal(err)
	}
	var err error
	app.document, err = OpenDocument(app.document.Path)
	if err != nil {
		t.Fatal(err)
	}
	app.pages, app.view, app.dirty = nil, viewChapters, false
	outline := app.document.chapterOutline()
	if len(app.document.Chapters) != 10 || outline.volumes != 3 || outline.chapters != 5 || outline.entries[0].chapterCount != 3 {
		t.Fatalf("duplicate grouping changed section IDs: %+v", outline)
	}
	rows, _ := app.directoryRows()
	if len(rows) != 3 {
		t.Fatalf("duplicate volume headers shown: %+v", rows)
	}
	app.handle(c1device.KeyRight)
	rows, _ = app.directoryRows()
	if len(rows) != 7 {
		t.Fatalf("expanded rows=%+v", rows)
	}
	if !app.openChapter(2, 0) || !app.volumeBodyPick || !app.expandedVolumes[0] || !strings.Contains(strings.Join(app.pages[0].Lines, ""), "补充说明") {
		t.Fatal("duplicate heading's introduction lost")
	}
	app.handle(c1device.KeyLeft)
	rows, selected := app.directoryRows()
	if rows[selected].chapter != 2 || !rows[selected].volumeBody {
		t.Fatal("continuation return selected wrong row")
	}
	app.handle(c1device.KeyLeft)
	if app.chapterPick != 0 || app.expandedVolumes[0] {
		t.Fatal("continuation did not collapse canonical volume")
	}
	if !app.openChapter(4, 0) || app.chapterIndex != 5 {
		t.Fatal("empty duplicate heading created blank page")
	}
	if got := app.document.contextualChapterTitle(7); !strings.HasPrefix(got, "第二卷 · ") {
		t.Fatalf("invented volume ordinal: %s", got)
	}
}

func TestTXTSeparatorsDoNotCreateFakeIntroductions(t *testing.T) {
	app := readingFixture(t)
	text := "\n------------\n\n第一卷 开篇\n\n------------\n第一章 开始\n正文\n第二卷 新卷\n———\n第二章 继续\n正文\n"
	if err := os.WriteFile(app.document.Path, []byte(text), 0644); err != nil {
		t.Fatal(err)
	}
	app.books = []Book{{Path: app.document.Path}}
	app.dirty = false
	app.openSelectedBook()
	if app.message != "" {
		t.Fatal(app.message)
	}
	if len(app.document.Chapters) != 5 || app.document.Chapters[0].HasBody || app.document.Chapters[1].HasBody || app.document.Chapters[3].HasBody {
		t.Fatal("separator-only introductions were classified as text")
	}
	rows, selected := app.directoryRows()
	if len(rows) != 2 || rows[selected].chapter != 1 || app.chapterPick != 1 {
		t.Fatalf("initial directory=%+v pick=%d", rows, app.chapterPick)
	}
	app.handle(c1device.KeyRight)
	app.handle(c1device.KeyDown)
	if app.chapterPick != 2 {
		t.Fatal("separator became volume body row")
	}
	if !app.jumpToPercent(0) || app.chapterIndex != 2 {
		t.Fatal("zero percent opened separator instead of first chapter")
	}
}

// Opt-in verification of a user's local TXT: scan it read-only without
// creating or changing a chapter cache next to the source book.
func TestProvidedBookVolumeOutline(t *testing.T) {
	path := os.Getenv("C1_TEST_BOOK_PATH")
	if path == "" {
		t.Skip("set C1_TEST_BOOK_PATH to validate a local book")
	}
	file, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		t.Fatal(err)
	}
	encoding, bom, err := detectEncoding(file)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := file.Seek(0, io.SeekStart); err != nil {
		t.Fatal(err)
	}
	chapters, err := scanChapters(file, info.Size(), encoding, bom)
	if err != nil {
		t.Fatal(err)
	}
	doc := &Document{Path: path, Encoding: encoding, Size: info.Size(), Chapters: chapters}
	outline := doc.chapterOutline()
	app := &readerApp{document: doc, view: viewChapters}
	rows, _ := app.directoryRows()
	if !validChapterIndex(chapters, info.Size()) {
		t.Fatal("noncontiguous source offsets")
	}
	t.Logf("raw sections=%d, volumes=%d, chapters=%d, collapsed rows=%d", len(chapters), outline.volumes, outline.chapters, len(rows))
	readerUIFaces(t, app)
	if len(rows) > 0 {
		app.chapterPick = rows[0].chapter
	}
	writeReaderPreview(t, "actual-book-volumes", app.render())
	if len(rows) > 0 {
		app.chapterPick = rows[0].chapter
		app.activateDirectorySelection()
		writeReaderPreview(t, "actual-book-expanded", app.render())
	}
	// Exercise every whole-percent target plus fractional edge cases through
	// real navigation and rendering, without writing progress to the source.
	app.pages = nil
	for units := 0; units <= maxPercentUnits; units += percentScale {
		app.view = viewChapters
		app.handleEvent(keyO())
		inputText(app, formatPercentUnits(units))
		app.handle(c1device.KeyOK)
		if app.view != viewReader || app.message != "" {
			t.Fatalf("jump %s%%: view=%v error=%q", formatPercentUnits(units), app.view, app.message)
		}
		app.render()
		app.handle(c1device.KeyDown)
		app.render()
		app.handle(c1device.KeyUp)
		app.render()
	}
	for _, text := range []string{"19.65", "99.99"} {
		app.handleEvent(keyO())
		inputText(app, text)
		app.handle(c1device.KeyOK)
		if app.view != viewReader || app.message != "" {
			t.Fatalf("decimal jump %s: %s", text, app.message)
		}
		writeReaderPreview(t, "actual-book-jump-"+text, app.render())
	}
}

func TestPlainChapterBookKeepsFlatDirectory(t *testing.T) {
	app := chapterJumpFixture(t)
	rows, _ := app.directoryRows()
	if len(rows) != 2337 || app.directoryCount() != "2337 章" || app.directoryHint() != chapterListHint {
		t.Fatal("plain chapter UI changed")
	}
	app.handle(c1device.KeyDown)
	app.handle(c1device.KeyRight)
	if app.chapterIndex != 1 || app.view != viewReader {
		t.Fatal("plain chapter navigation changed")
	}
}
