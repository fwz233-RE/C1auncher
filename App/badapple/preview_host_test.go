//go:build !linux || !mipsle

package main

import (
	"context"
	"image/png"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestPreviewAndOverwriteProtection(t *testing.T) {
	path := testAsset(t, 2, 2, 1)
	output := filepath.Join(t.TempDir(), "frame.png")
	if err := writePreview(path, output); err != nil {
		t.Fatal(err)
	}
	f, err := os.Open(output)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	img, err := png.Decode(f)
	if err != nil {
		t.Fatal(err)
	}
	if img.Bounds().Dx() != width*3 || img.Bounds().Dy() != height*3 {
		t.Fatal(img.Bounds())
	}
	if err := writePreview(path, path); err == nil {
		t.Fatal("overwrote source")
	}
	a, err := openAsset(path)
	if err != nil {
		t.Fatal(err)
	}
	a.close()
}

func TestEmbeddedPreviewWithoutFile(t *testing.T) {
	t.Setenv("C1_BADAPPLE_FILE", "")
	t.Chdir(t.TempDir())
	if err := run([]string{"--preview", "frame.png", "--preview-frame", "120"}); err != nil {
		t.Fatal(err)
	}
	f, err := os.Open("frame.png")
	if err != nil {
		t.Fatal(err)
	}
	img, err := png.Decode(f)
	_ = f.Close()
	if err != nil {
		t.Fatal(err)
	}
	a, err := openAsset("")
	if err != nil {
		t.Fatal(err)
	}
	defer a.close()
	want, err := a.read(120)
	if err != nil {
		t.Fatal(err)
	}
	for y := 0; y < height; y++ {
		for x := 0; x < width; x++ {
			r, _, _, _ := img.At(x*3, y*3).RGBA()
			black := want[(y/8)*width+x]&(0x80>>uint(y%8)) != 0
			if (r == 0) != black {
				t.Fatalf("pixel mismatch at %d,%d", x, y)
			}
		}
	}
	// Existing preview targets must not dereference a nonexistent source file.
	if err := writePreviewAt("", "frame.png", 120); err != nil {
		t.Fatal(err)
	}
}

func TestSlowWritesSkipOverdueFrames(t *testing.T) {
	d := &fakeDisplay{keys: make(chan uint16)}
	withDisplay(t, d)
	// Block after first motion frame; the next frame must be selected by time,
	// not by incrementing the previous index or replaying pending frames.
	d.hook = func() {
		if len(d.drawn) == 2 {
			time.Sleep(120 * time.Millisecond)
		}
	}
	ctx, cancel := context.WithTimeout(context.Background(), 240*time.Millisecond)
	defer cancel()
	err := play(ctx, options{path: testAsset(t, 100, 30, 1), interval: time.Millisecond, loop: true})
	if err != nil {
		t.Fatal(err)
	}
	if len(d.drawn) < 3 || int(d.drawn[2][0])-int(d.drawn[1][0]) < 3 {
		t.Fatalf("failed to skip stale frames: %d draws", len(d.drawn))
	}
	if !d.closed {
		t.Fatal("deadline leaked display")
	}
	for _, full := range d.full[1:] {
		if full {
			t.Fatal("automatic full during playback")
		}
	}
}
