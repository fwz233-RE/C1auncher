package main

import "testing"

func TestCanonicalPicturesDirectory(t *testing.T) {
	if defaultPicturesDir != "/storage/mtp/Pic" {
		t.Fatalf("pictures directory = %q", defaultPicturesDir)
	}
}
