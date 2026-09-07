package main

import (
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

func TestScanLibraryRecursiveStableSort(t *testing.T) {
	root := t.TempDir()
	for _, name := range []string{"z.txt", filepath.Join("nested", "B.TXT"), "a.txt", "skip.md"} {
		path := filepath.Join(root, name)
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte("text"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(filepath.Join(root, "a.txt.name"), []byte("蛊真人.txt"), 0o644); err != nil {
		t.Fatal(err)
	}
	books, err := ScanLibrary(root)
	if err != nil {
		t.Fatal(err)
	}
	got := make([]string, len(books))
	for index := range books {
		got[index] = books[index].Name
	}
	want := []string{"蛊真人.txt", "B.TXT", "z.txt"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("names = %#v, want %#v", got, want)
	}
}
