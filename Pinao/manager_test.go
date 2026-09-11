package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestManagedArchivesAndExports(t *testing.T) {
	dir := t.TempDir()
	song := filepath.Join(dir, "song.json")
	if _, err := createArchive(song, "practice"); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(dir, "exports"), 0700); err != nil {
		t.Fatal(err)
	}
	wav := filepath.Join(dir, "exports", "take.wav")
	if err := os.WriteFile(wav, []byte("wav"), 0600); err != nil {
		t.Fatal(err)
	}
	got, err := listManagedFiles(song)
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != 2 || got[0].IsWAV {
		t.Fatalf("unexpected managed files: %+v", got)
	}
	if err := deleteManagedFile(wav); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(wav); !os.IsNotExist(err) {
		t.Fatalf("wav still exists: %v", err)
	}
}

func TestArchivePathRejectsTraversal(t *testing.T) {
	if _, err := archivePath(filepath.Join(t.TempDir(), "song.json"), "../escape"); err == nil {
		t.Fatal("expected traversal rejection")
	}
}
