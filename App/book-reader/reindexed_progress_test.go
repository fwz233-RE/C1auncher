package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestExpandedHeadingIndexPreservesOldTXTProgress(t *testing.T) {
	root := t.TempDir()
	path := filepath.Join(root, "reindexed.txt")
	prefix := "书名\n〖第一章 起始〗\n第一段正文。\n"
	body := "〖第二章 后续〗\n第二段正文。\n"
	if err := os.WriteFile(path, []byte(prefix+body), 0600); err != nil {
		t.Fatal(err)
	}
	doc, err := OpenDocument(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(doc.Chapters) < 3 {
		t.Fatalf("wrapped headings not indexed: %d", len(doc.Chapters))
	}
	offset := int64(len(prefix) + len("〖第二章 后续〗\n"))
	mark, err := fingerprint(path)
	if err != nil {
		t.Fatal(err)
	}
	store := ProgressStore{Dir: filepath.Join(root, "state")}
	if err := store.Save(Progress{Path: path, Fingerprint: mark, Chapter: 0, Offset: offset}); err != nil {
		t.Fatal(err)
	}
	ui, _ := newReaderFace(false)
	defer ui.Close()
	bodyFace, _ := newReaderFace(true)
	defer bodyFace.Close()
	app, err := newReaderApp(root, ui, bodyFace, store, BookmarkStore{Dir: store.Dir})
	if err != nil {
		t.Fatal(err)
	}
	app.openSelectedBook()
	if app.chapterIndex != 2 || app.resumeOffset != offset {
		t.Fatalf("old position was not reindexed: chapter %d offset %d", app.chapterIndex, app.resumeOffset)
	}
	bookmarks := validBookmarks(doc, []Bookmark{{Chapter: 0, Offset: offset}})
	if len(bookmarks) != 1 || bookmarks[0].Chapter != 2 || bookmarks[0].Offset != offset {
		t.Fatalf("old bookmark lost: %+v", bookmarks)
	}
}
