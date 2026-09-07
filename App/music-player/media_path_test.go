package main

import "testing"

func TestCanonicalMusicDirectory(t *testing.T) {
	if defaultMusicDir != "/storage/mtp/Music" {
		t.Fatalf("music directory = %q", defaultMusicDir)
	}
}
