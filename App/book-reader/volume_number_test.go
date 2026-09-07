package main

import (
	"fmt"
	"os"
	"reflect"
	"strings"
	"testing"

	"c1device"
)

func TestVolumeNumberForms(t *testing.T) {
	for _, tc := range []struct {
		title  string
		number int
		unit   string
	}{
		{"第一卷 开始", 1, "卷"}, {"第2卷：继续", 2, "卷"}, {"第３卷 后续", 3, "卷"},
		{"第壹卷 开始", 1, "卷"}, {"第貳卷 继续", 2, "卷"}, {"第參卷 后续", 3, "卷"},
		{"第贰拾壹卷 后续", 21, "卷"}, {"第一百零二卷 后续", 102, "卷"},
		{"卷一：开始", 1, "卷"}, {"卷02 继续", 2, "卷"}, {"卷叁 后续", 3, "卷"},
		{"第两部 继续", 2, "部"}, {"第一千零一篇 后续", 1001, "篇"}, {"第一〇二卷 后续", 102, "卷"},
	} {
		number, unit, ok := volumeNumber(tc.title)
		if !ok || number != tc.number || unit != tc.unit {
			t.Errorf("%q=%d,%s,%v want %d,%s", tc.title, number, unit, ok, tc.number, tc.unit)
		}
	}
	for _, text := range []string{"第零卷", "第0卷", "第十十卷", "第一百二卷", "第abc卷", "第二十一章 第三卷", "第10000卷", "卷终", "第一百百卷"} {
		if _, _, ok := volumeNumber(text); ok {
			t.Errorf("invalid/ambiguous volume accepted: %q", text)
		}
	}
}

func TestChineseVolumeNumberRoundTrip(t *testing.T) {
	for number := 1; number <= 9999; number++ {
		got, ok := parseVolumeNumber(chineseNumber(number))
		if !ok || got != number {
			t.Fatalf("%s=%d,%v want=%d", chineseNumber(number), got, ok, number)
		}
	}
}

func outlineFixture(t *testing.T, headings []string) *readerApp {
	t.Helper()
	app := readingFixture(t)
	var text strings.Builder
	for _, heading := range headings {
		fmt.Fprintf(&text, "%s\n说明文字。\n第一章 正文\n第一行正文。\n第二行正文。\n", heading)
	}
	if err := os.WriteFile(app.document.Path, []byte(text.String()), 0644); err != nil {
		t.Fatal(err)
	}
	document, err := OpenDocument(app.document.Path)
	if err != nil {
		t.Fatal(err)
	}
	if len(document.Chapters) != len(headings)*2 {
		t.Fatalf("fixture scanner missed heading: %v => %d", headings, len(document.Chapters))
	}
	app.document, app.pages, app.view, app.dirty = document, nil, viewChapters, false
	app.books = []Book{{Path: document.Path}}
	return app
}

func TestOnlyContinuousVolumesEnableGrouping(t *testing.T) {
	for _, tc := range []struct {
		name    string
		titles  []string
		grouped bool
		volumes int
	}{
		{"Chinese", []string{"第一卷 开始", "第二卷 中段", "第三卷 结束"}, true, 3},
		{"Arabic", []string{"第1卷 开始", "第2卷 中段", "第3卷 结束"}, true, 3},
		{"Financial", []string{"第壹卷 开始", "第贰卷 中段", "第叁卷 结束"}, true, 3},
		{"Mixed", []string{"第一卷 开始", "第２卷 中段", "第參卷 结束"}, true, 3},
		{"Reverse", []string{"卷一 开始", "卷2 中段", "卷三 结束"}, true, 3},
		{"RepeatedSame", []string{"第一卷：开始", "第1卷 ：开始", "第壹卷:开始", "第二卷 中段"}, true, 2},
		{"Missing", []string{"第一卷 开始", "第三卷 中段", "第六卷 结束"}, false, 0},
		{"MissingFirst", []string{"第二卷 中段", "第三卷 结束"}, false, 0},
		{"Descending", []string{"第一卷 开始", "第三卷 中段", "第二卷 结束"}, false, 0},
		{"Reset", []string{"第一卷 开始", "第二卷 中段", "第一卷 开始"}, false, 0},
		{"Ambiguous", []string{"第一卷 开始", "第abc卷 中段"}, false, 0},
		{"Zero", []string{"第零卷 开始", "第一卷 中段"}, false, 0},
		{"DuplicateDifferentTitle", []string{"第一卷 开始", "第一卷 别名", "第二卷 结束"}, false, 0},
		{"MixedLevels", []string{"第一部 开始", "第二卷 中段"}, false, 0},
		{"SingleFirst", []string{"第一卷 开始"}, true, 1},
		{"SingleSixth", []string{"第六卷 开始"}, false, 0},
	} {
		t.Run(tc.name, func(t *testing.T) {
			app := outlineFixture(t, tc.titles)
			original := append([]Chapter(nil), app.document.Chapters...)
			outline := app.document.chapterOutline()
			if outline.grouped != tc.grouped || outline.volumes != tc.volumes {
				t.Fatalf("grouped=%v volumes=%d", outline.grouped, outline.volumes)
			}
			rows, _ := app.directoryRows()
			if tc.grouped {
				if len(rows) != tc.volumes || !app.selectedVolumeHeader() {
					t.Fatal("valid hierarchy did not collapse")
				}
				app.handle(c1device.KeyOK)
				if app.view != viewChapters || !app.expandedVolumes[0] {
					t.Fatal("OK failed to expand volume")
				}
				app.handle(c1device.KeyDown)
				app.handle(c1device.KeyOK)
				if app.view != viewReader {
					t.Fatal("OK failed to read introduction")
				}
			} else {
				if len(rows) != len(original) || app.selectedVolumeHeader() || strings.Contains(app.directoryCount(), "卷") {
					t.Fatalf("invalid hierarchy exposed folders: %+v", rows)
				}
				for index, entry := range outline.entries {
					if entry.parent != -1 {
						t.Fatalf("stale parent at %d", index)
					}
					if strings.Contains(app.document.contextualChapterTitle(index), " · ") {
						t.Fatal("false parent in reader/bookmark heading")
					}
				}
				app.handle(c1device.KeyDown)
				app.handle(c1device.KeyOK)
				if app.view != viewReader || app.chapterIndex != 1 {
					t.Fatal("flat OK did not open chapter")
				}
				app.handle(c1device.KeyLeft)
				rows, selected := app.directoryRows()
				if rows[selected].chapter != 1 || len(app.expandedVolumes) != 0 {
					t.Fatal("flat resume revealed a volume")
				}
				app.handle(c1device.KeyLeft)
				if app.view != viewShelf {
					t.Fatal("flat back tried to collapse")
				}
			}
			if !reflect.DeepEqual(original, app.document.Chapters) {
				t.Fatal("hierarchy decision changed saved chapter IDs or offsets")
			}
		})
	}
}

func TestInvalidGroupingKeepsLegacyProgressBookmarksAndCache(t *testing.T) {
	app := outlineFixture(t, []string{"第一卷 开始", "第三卷 中段", "第六卷 结束"})
	doc := app.document
	mark, err := fingerprint(doc.Path)
	if err != nil {
		t.Fatal(err)
	}
	chapter := 3
	offset := doc.Chapters[chapter].Start + int64(len("第一章 正文\n"))
	if err := app.store.Save(Progress{Path: doc.Path, Fingerprint: mark, Chapter: chapter, Offset: offset}); err != nil {
		t.Fatal(err)
	}
	bookmarks := []Bookmark{{Chapter: chapter, Offset: offset}, {Chapter: 4, Offset: doc.Chapters[4].Start}}
	if err := app.bookmarkStore.Save(doc.Path, bookmarks); err != nil {
		t.Fatal(err)
	}
	// OpenDocument must take the existing v2 cache but recompute grouping.
	app.openSelectedBook()
	if app.document.chapterOutline().grouped || app.chapterIndex != chapter || app.resumeOffset != offset || len(app.expandedVolumes) != 0 {
		t.Fatal("legacy progress failed flat fallback")
	}
	if !reflect.DeepEqual(bookmarks, app.bookmarks) {
		t.Fatal("fallback lost bookmarks")
	}
	app.handle(c1device.KeyOK)
	if app.pages[app.pageIndex].Start != offset {
		t.Fatal("resume changed byte offset")
	}
	app.handle(c1device.KeyLeft)
	app.handle(c1device.KeyPause)
	app.handle(c1device.KeyOK)
	app.handleEvent(keyO())
	inputText(app, "99.99")
	app.handle(c1device.KeyOK)
	if app.view != viewReader || app.readerOrigin != viewBookmarks || len(app.expandedVolumes) != 0 {
		t.Fatal("bookmark percent jump lost flat state")
	}
	app.handle(c1device.KeyLeft)
	if app.view != viewBookmarks {
		t.Fatal("bookmark origin lost")
	}
}
