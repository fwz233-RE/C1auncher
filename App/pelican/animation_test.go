package main

import (
	"bytes"
	"context"
	"errors"
	"image"
	"image/color"
	"image/gif"
	"os"
	"path/filepath"
	"testing"
	"time"

	"c1device"
)

func TestFrames(t *testing.T) {
	frames := animationFrames()
	seen := map[c1device.Frame]bool{}
	for i, f := range frames {
		if seen[f] {
			t.Fatalf("duplicate frame %d", i)
		}
		seen[f] = true
		if f != pack(render(i+frameCount)) {
			t.Fatal("loop not periodic")
		}
		img := render(i)
		if img.Bounds() != image.Rect(0, 0, 296, 152) {
			t.Fatal("dimensions")
		}
		black := 0
		for _, v := range img.Pix {
			if v == 0 {
				black++
			} else if v != 255 {
				t.Fatal("unexpected grayscale")
			}
		}
		if black < 1500 || black > 18000 {
			t.Fatalf("unexpected ink coverage %d", black)
		}
	}
}

func TestPacking(t *testing.T) {
	img := image.NewGray(image.Rect(0, 0, 296, 152))
	for i := range img.Pix {
		img.Pix[i] = 255
	}
	for _, p := range []image.Point{{0, 0}, {0, 7}, {295, 8}, {295, 151}} {
		img.SetGray(p.X, p.Y, color.Gray{Y: 0})
	}
	f := pack(img)
	if len(f) != 5624 || f[0] != 0x81 || f[591] != 0x80 || f[5623] != 0x01 {
		t.Fatal("incorrect vertical packing")
	}
}

func TestPreviewAndCLI(t *testing.T) {
	path := filepath.Join(t.TempDir(), "pelican.gif")
	var out bytes.Buffer
	if err := run([]string{"--preview", path}, &out); err != nil {
		t.Fatal(err)
	}
	file, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	g, err := gif.DecodeAll(file)
	if err != nil {
		t.Fatal(err)
	}
	if len(g.Image) != frameCount || g.LoopCount != 0 || g.Delay[0] != 75 || g.Config.Width != 888 || g.Config.Height != 456 {
		t.Fatal("invalid preview")
	}
	if err := run([]string{"--version"}, &out); err != nil {
		t.Fatal(err)
	}
	if !bytes.Contains(out.Bytes(), []byte("pelican ")) {
		t.Fatal("version missing")
	}
	if err := run([]string{"--interval", "0ms"}, &out); err == nil {
		t.Fatal("accepted zero interval")
	}
}

type fakePlatform struct {
	events chan c1device.Event
	calls  chan bool
	err    error
}

func (p *fakePlatform) Draw(_ c1device.Frame, full bool) error { p.calls <- full; return p.err }
func (p *fakePlatform) Events() <-chan c1device.Event          { return p.events }
func (p *fakePlatform) Close() error                           { return nil }

func TestPlayback(t *testing.T) {
	p := &fakePlatform{events: make(chan c1device.Event, 1), calls: make(chan bool, 8)}
	frames := animationFrames()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	done := make(chan error, 1)
	go func() { done <- play(ctx, p, &frames, time.Millisecond) }()
	for i := 0; i < 3; i++ {
		select {
		case full := <-p.calls:
			if full != (i == 0) {
				t.Fatal("only first frame should request full refresh")
			}
		case <-ctx.Done():
			t.Fatal("playback stalled")
		}
	}
	p.events <- c1device.Event{Key: c1device.KeyBack}
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-ctx.Done():
		t.Fatal("exit stalled")
	}
}

func TestDrawError(t *testing.T) {
	want := errors.New("screen failed")
	p := &fakePlatform{events: make(chan c1device.Event), calls: make(chan bool, 1), err: want}
	var frames [frameCount]c1device.Frame
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	if err := play(ctx, p, &frames, time.Millisecond); !errors.Is(err, want) {
		t.Fatalf("lost draw error: %v", err)
	}
}
