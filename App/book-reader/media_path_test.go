package main

import "testing"

func TestCanonicalBooksDirectory(t *testing.T) {
	if defaultBooksDir != "/storage/mtp/Book" {
		t.Fatalf("books directory = %q", defaultBooksDir)
	}
}
