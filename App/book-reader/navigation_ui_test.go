package main

import (
	"c1device"
	"strings"
	"testing"
)

func TestVolumeKeysFollowPhysicalNavigation(t *testing.T) {
	app := chapterJumpFixture(t)
	app.handle(c1device.KeyVolumeUp)
	if app.chapterPick != 1 {
		t.Fatalf("volume+ must move down: %d", app.chapterPick)
	}
	app.handle(c1device.KeyVolumeDown)
	if app.chapterPick != 0 {
		t.Fatal("volume- must move up")
	}
	app.view = viewShelf
	app.books = []Book{{Name: "one"}, {Name: "two"}}
	app.handle(c1device.KeyVolumeUp)
	if app.bookIndex != 1 {
		t.Fatal("shelf volume+")
	}
	app.handle(c1device.KeyVolumeDown)
	if app.bookIndex != 0 {
		t.Fatal("shelf volume-")
	}
	app.view = viewBookmarks
	app.bookmarks = []Bookmark{{Chapter: 0, Offset: 0}, {Chapter: 1, Offset: app.document.Chapters[1].Start}}
	app.handle(c1device.KeyVolumeUp)
	if app.bookmarkPick != 1 {
		t.Fatal("bookmark volume+")
	}
	app.handle(c1device.KeyVolumeDown)
	if app.bookmarkPick != 0 {
		t.Fatal("bookmark volume-")
	}
	app.view = viewReader
	app.pages = []Page{{Start: 0, End: 1}, {Start: 1, End: 2}}
	app.pageIndex = 0
	app.handle(c1device.KeyVolumeUp)
	if app.pageIndex != 1 {
		t.Fatal("volume+ must advance page")
	}
	app.handle(c1device.KeyVolumeDown)
	if app.pageIndex != 0 {
		t.Fatal("volume- must go back a page")
	}
}

func TestBookmarkJumpShortcutAndOrigin(t *testing.T) {
	app := chapterJumpFixture(t)
	app.view = viewBookmarks
	app.bookmarks = []Bookmark{{Chapter: 1, Offset: app.document.Chapters[1].Start}}
	app.handleEvent(keyO())
	if app.view != viewPercentJump || app.percentOrigin != viewBookmarks {
		t.Fatal("O shortcut missing")
	}
	app.handle(c1device.KeyBack)
	if app.view != viewBookmarks {
		t.Fatal("cancel lost bookmark origin")
	}
	app.handleEvent(keyO())
	app.handle(c1device.KeyOK)
	if app.view != viewReader || app.readerOrigin != viewBookmarks {
		t.Fatal("jump lost bookmark return")
	}
	app.handle(c1device.KeyLeft)
	if app.view != viewBookmarks {
		t.Fatal("left did not return to bookmarks")
	}
}
func TestFourSeparateNavigationCues(t *testing.T) {
	app := chapterJumpFixture(t)
	readerUIFaces(t, app)
	for _, hint := range []string{chapterListHint, bookmarkListHint, app.directoryHint()} {
		labels := strings.Fields(hint)
		if len(labels) != 4 {
			t.Fatalf("four separate cues required: %s", hint)
		}
		for i, prefix := range []string{"←", "↑", "↓", "→"} {
			if !strings.HasPrefix(labels[i], prefix) || app.uiFace.Measure(labels[i]) > 48 {
				t.Fatalf("navigation slot %d: %s", i, labels[i])
			}
		}
	}
	app.view = viewChapters
	writeReaderPreview(t, "new-chapter-navigation", app.render())
	app.view = viewBookmarks
	app.bookmarks = []Bookmark{{Chapter: 0, Offset: app.document.Chapters[0].Start}}
	writeReaderPreview(t, "new-bookmark-navigation", app.render())
	app = chapterPagesFixture(t, "第一章 章节页码显示示例\n"+strings.Repeat("这是用于预览分页的示例文字。\n", 123))
	readerUIFaces(t, app)
	if !app.openChapter(0, app.document.Chapters[0].Start) {
		t.Fatal(app.message)
	}
	app.nextPage()
	app.nextPage()
	writeReaderPreview(t, "new-chapter-page-count", app.render())
}
