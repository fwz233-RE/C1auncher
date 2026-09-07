package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestProgressSaveRestoreAndFingerprint(t *testing.T) {
	root := t.TempDir()
	book := filepath.Join(root, "book.txt")
	if err := os.WriteFile(book, []byte("content"), 0o644); err != nil {
		t.Fatal(err)
	}
	mark, err := fingerprint(book)
	if err != nil {
		t.Fatal(err)
	}
	store := ProgressStore{Dir: filepath.Join(root, "state")}
	progress := Progress{Fingerprint: mark, Path: book, Chapter: 3, Offset: 123}
	if err := store.Save(progress); err != nil {
		t.Fatal(err)
	}
	progress.Offset = 456
	if err := store.Save(progress); err != nil {
		t.Fatal(err)
	}
	loaded, ok, err := store.Load(book)
	if err != nil {
		t.Fatal(err)
	}
	if !ok || loaded.Chapter != 3 || loaded.Offset != 456 || loaded.LayoutVersion != layoutVersion {
		t.Fatalf("loaded = %#v, ok = %v", loaded, ok)
	}
	if err := os.WriteFile(book, []byte("changed content"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, ok, err := store.Load(book); err != nil || ok {
		t.Fatalf("changed fingerprint accepted: ok=%v err=%v", ok, err)
	}
}
