//go:build !linux || !mipsle

package main

import (
	"image/png"
	"os"
	"path/filepath"
	"testing"
)

func TestHostPreview(t *testing.T) {
	old := createDisplay
	createDisplay = func() (display, error) { t.Fatal("preview opened display"); return nil, nil }
	defer func() { createDisplay = old }()
	for _, pattern := range []string{"move", "flip", "scene"} {
		path := filepath.Join(t.TempDir(), "preview.png")
		if err := run([]string{"--preview", path, "--pattern", pattern}); err != nil {
			t.Fatal(err)
		}
		f, err := os.Open(path)
		if err != nil {
			t.Fatal(err)
		}
		img, err := png.Decode(f)
		_ = f.Close()
		if err != nil {
			t.Fatal(err)
		}
		if img.Bounds().Dx() != width*3 || img.Bounds().Dy() != height*3 {
			t.Fatal(img.Bounds())
		}
	}
}
func TestHelpPreview(t *testing.T) {
	if err := writePreview(filepath.Join(t.TempDir(), "help.png"), helpFrame()); err != nil {
		t.Fatal(err)
	}
}
