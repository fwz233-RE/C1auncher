package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// Saved positions are source byte offsets, not a page number or font size.
// A switch from the prior TrueType layout must retain that reading position.
func TestNativeFontResumesExistingBytePosition(t *testing.T) {
	root := t.TempDir()
	path := filepath.Join(root, "resume.txt")
	content := "第一章 测试\n" + strings.Repeat("原来的阅读位置应当保留。", 100) + "\n"
	if err := os.WriteFile(path, []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	mark, err := fingerprint(path)
	if err != nil {
		t.Fatal(err)
	}
	offset := int64(len("第一章 测试\n") + len("原来的阅读位置应当保留。")*7)
	store := ProgressStore{Dir: filepath.Join(root, "state")}
	if err := store.Save(Progress{Path: path, Fingerprint: mark, Chapter: 0, Offset: offset, LayoutVersion: 3}); err != nil {
		t.Fatal(err)
	}
	ui, err := newReaderFace(false)
	if err != nil {
		t.Fatal(err)
	}
	defer ui.Close()
	body, err := newReaderFace(true)
	if err != nil {
		t.Fatal(err)
	}
	defer body.Close()
	app, err := newReaderApp(root, ui, body, store, BookmarkStore{Dir: store.Dir})
	if err != nil {
		t.Fatal(err)
	}
	app.openSelectedBook()
	if app.resumeOffset != offset {
		t.Fatalf("resume offset lost: got %d, want %d", app.resumeOffset, offset)
	}
	if !app.openChapter(0, app.resumeOffset) {
		t.Fatal(app.message)
	}
	if app.pages[0].Start != offset {
		t.Fatalf("native layout changed saved start: %d", app.pages[0].Start)
	}
	if err := app.saveProgress(); err != nil {
		t.Fatal(err)
	}
	got, ok, err := store.Load(path)
	if err != nil || !ok || got.Offset != offset {
		t.Fatalf("save/load: %+v %v %v", got, ok, err)
	}
}
