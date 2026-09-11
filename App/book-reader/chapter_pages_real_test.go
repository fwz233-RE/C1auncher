package main

import "testing"

// Sample the beginning, middle and end of each supplied book with the actual
// device font. Keep source books read-only and report no book text.
func TestSuppliedBooksChapterPageNavigation(t *testing.T) {
	_, paths := suppliedBooks(t)
	t.Setenv("C1BOOK_READER_CACHE_DIR", t.TempDir())
	for book, path := range paths {
		before := fileDigest(t, path)
		app := readingFixture(t)
		readerUIFaces(t, app)
		doc, err := OpenDocument(path)
		if err != nil {
			t.Fatal(err)
		}
		app.document = doc
		for _, candidate := range []int{0, len(doc.Chapters) / 2, len(doc.Chapters) - 1} {
			ci, ok := doc.readableChapter(candidate, 1)
			if !ok {
				continue
			}
			ch := doc.Chapters[ci]
			if !app.openChapter(ci, ch.Start) {
				t.Fatalf("book %d chapter %d: %s", book, ci, app.message)
			}
			index := app.chapterPagination
			total := len(index.starts)
			assertChapterPage(t, app, 1, total)
			middle := total / 2
			if !app.openChapter(ci, index.starts[middle]) {
				t.Fatalf("book %d: %s", book, app.message)
			}
			assertChapterPage(t, app, middle+1, total)
			bookmark, ok := app.currentBookmark()
			if !ok {
				t.Fatal("missing bookmark")
			}
			app.bookmarks = []Bookmark{bookmark}
			app.bookmarkPick = 0
			app.openSelectedBookmark()
			assertChapterPage(t, app, middle+1, total)
			if middle+1 < total {
				app.nextPage()
				assertChapterPage(t, app, middle+2, total)
				app.previousPage()
				assertChapterPage(t, app, middle+1, total)
			}
			if !app.openChapter(ci, index.starts[total-1]) {
				t.Fatal(app.message)
			}
			assertChapterPage(t, app, total, total)
		}
		if before != fileDigest(t, path) {
			t.Fatal("source book modified")
		}
		t.Logf("book %d: native chapter page counts, bookmarks and navigation PASS", book)
	}
}
