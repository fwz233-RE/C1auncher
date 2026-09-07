package main

import (
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

func TestSupportedTrackExtensions(t *testing.T) {
	for _, path := range []string{"song.mp3", "song.MP3", "song.flac", "song.FLAC"} {
		if !isSupportedTrack(path) {
			t.Errorf("%q was not recognized as a supported track", path)
		}
	}
	for _, path := range []string{"song.wav", "song.ogg", "song.flac.txt", "song"} {
		if isSupportedTrack(path) {
			t.Errorf("%q was incorrectly recognized as a supported track", path)
		}
	}
}

func TestScanLibraryRecursivelyAndSorts(t *testing.T) {
	root := t.TempDir()
	files := []string{
		filepath.Join(root, "zeta.MP3"),
		filepath.Join(root, "Album", "beta.mp3"),
		filepath.Join(root, "Album", "Alpha.Mp3"),
		filepath.Join(root, "Album", "ambient.FLAC"),
		filepath.Join(root, "Album", "live.FlAc"),
		filepath.Join(root, "ignore.wav"),
	}
	for _, path := range files {
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, nil, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(filepath.Join(root, "zeta.MP3.name"), []byte("川井憲次 - 謡I.mp3"), 0o644); err != nil {
		t.Fatal(err)
	}

	tracks, err := scanLibrary(root)
	if err != nil {
		t.Fatal(err)
	}
	var titles []string
	for _, track := range tracks {
		titles = append(titles, track.Title)
	}
	want := []string{"Alpha", "ambient", "beta", "live", "川井憲次 - 謡I"}
	if !reflect.DeepEqual(titles, want) {
		t.Fatalf("titles = %v, want %v", titles, want)
	}
}
