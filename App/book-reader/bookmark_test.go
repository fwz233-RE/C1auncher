package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestBookmarkStoreSaveLoadAndDelete(t *testing.T) {
	root := t.TempDir()
	book := filepath.Join(root, "book.txt")
	if err := os.WriteFile(book, []byte("chapter content"), 0o644); err != nil {
		t.Fatal(err)
	}
	store := BookmarkStore{Dir: filepath.Join(root, "state")}
	bookmarks := []Bookmark{{Chapter: 1, Offset: 90}, {Chapter: 0, Offset: 12}, {Chapter: 0, Offset: 12}}
	if err := store.Save(book, bookmarks); err != nil {
		t.Fatal(err)
	}
	loaded, err := store.Load(book)
	if err != nil {
		t.Fatal(err)
	}
	if len(loaded) != 2 || loaded[0] != (Bookmark{Chapter: 0, Offset: 12}) || loaded[1] != (Bookmark{Chapter: 1, Offset: 90}) {
		t.Fatalf("loaded = %#v", loaded)
	}
	if err := store.Save(book, nil); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(store.bookmarkPath(book)); !os.IsNotExist(err) {
		t.Fatalf("bookmark file still exists: %v", err)
	}
}

func TestBookmarkStoreRejectsChangedBookAndCorruptData(t *testing.T) {
	root := t.TempDir()
	book := filepath.Join(root, "book.txt")
	if err := os.WriteFile(book, []byte("content"), 0o644); err != nil {
		t.Fatal(err)
	}
	store := BookmarkStore{Dir: filepath.Join(root, "state")}
	if err := store.Save(book, []Bookmark{{Chapter: 0, Offset: 1}}); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(book, []byte("changed content"), 0o644); err != nil {
		t.Fatal(err)
	}
	loaded, err := store.Load(book)
	if err != nil || len(loaded) != 0 {
		t.Fatalf("changed book loaded bookmarks %#v, err=%v", loaded, err)
	}

	if err := os.MkdirAll(store.Dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(store.bookmarkPath(book), []byte("not json"), 0o644); err != nil {
		t.Fatal(err)
	}
	loaded, err = store.Load(book)
	if err != nil || len(loaded) != 0 {
		t.Fatalf("corrupt file loaded bookmarks %#v, err=%v", loaded, err)
	}
	if _, err := os.Stat(store.bookmarkPath(book)); !os.IsNotExist(err) {
		t.Fatalf("corrupt bookmark file was not removed: %v", err)
	}
}

func TestValidBookmarksRejectsOutOfRangePositions(t *testing.T) {
	document := &Document{Chapters: []Chapter{{Start: 10, End: 20}, {Start: 20, End: 30}}}
	got := validBookmarks(document, []Bookmark{
		{Chapter: -1, Offset: 12},
		{Chapter: 0, Offset: 9},
		{Chapter: 0, Offset: 10},
		{Chapter: 1, Offset: 29},
		{Chapter: 1, Offset: 30},
	})
	want := []Bookmark{{Chapter: 0, Offset: 10}, {Chapter: 1, Offset: 29}}
	if len(got) != len(want) || got[0] != want[0] || got[1] != want[1] {
		t.Fatalf("valid bookmarks = %#v, want %#v", got, want)
	}
}
