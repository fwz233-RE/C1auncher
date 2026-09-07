package main

import (
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"testing"
)

func TestFitImageRectPreservesAspectRatio(t *testing.T) {
	area := image.Rect(10, 26, 286, 127)
	got := fitImageRect(image.Rect(0, 0, 400, 200), area)
	if got.Dx() != 202 || got.Dy() != 101 || got.Min.X != 47 || got.Min.Y != 26 {
		t.Fatalf("fitImageRect() = %v", got)
	}
}

func TestMovePictureSelectionWraps(t *testing.T) {
	if got := movePictureSelection(0, 3, -1); got != 2 {
		t.Fatalf("previous selection = %d", got)
	}
	if got := movePictureSelection(2, 3, 1); got != 0 {
		t.Fatalf("next selection = %d", got)
	}
}

func TestLoadPictureScalesToPanel(t *testing.T) {
	path := filepath.Join(t.TempDir(), "sample.png")
	file, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	source := image.NewRGBA(image.Rect(0, 0, 400, 200))
	source.Set(0, 0, color.Black)
	if err := png.Encode(file, source); err != nil {
		_ = file.Close()
		t.Fatal(err)
	}
	if err := file.Close(); err != nil {
		t.Fatal(err)
	}
	got, err := loadPicture(path)
	if err != nil {
		t.Fatal(err)
	}
	if got.Bounds() != image.Rect(0, 0, 296, 148) {
		t.Fatalf("scaled bounds = %v", got.Bounds())
	}
}
