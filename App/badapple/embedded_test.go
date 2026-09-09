package main

import (
	"context"
	"os"
	"strings"
	"testing"
	"time"
)

func TestEmbeddedAnimation(t *testing.T) {
	a, err := openAsset("")
	if err != nil {
		t.Fatal(err)
	}
	defer a.close()
	if a.file != nil {
		t.Fatal("built-in animation opened an external file")
	}
	if a.count != 439 || a.fpsNum != 2 || a.fpsDen != 1 || a.duration() != 219500*time.Millisecond {
		t.Fatalf("unexpected bundled animation metadata: %+v", a)
	}
	var first frame
	motion := false
	for i := uint32(0); i < a.count; i++ {
		f, err := a.read(i)
		if err != nil {
			t.Fatalf("frame %d: %v", i, err)
		}
		if i == 0 {
			first = f
		} else if f != first {
			motion = true
		}
	}
	if !motion {
		t.Fatal("bundled animation has no motion")
	}
	if _, err := a.read(a.count); err == nil {
		t.Fatal("out-of-range embedded frame accepted")
	}
}

func TestDefaultLaunchWithoutExternalMedia(t *testing.T) {
	t.Setenv("C1_BADAPPLE_FILE", "")
	t.Chdir(t.TempDir())
	if defaultAsset() != "" {
		t.Fatal("default launch requires a file")
	}
	d := &fakeDisplay{keys: make(chan uint16, 1)}
	d.keys <- 16
	withDisplay(t, d)
	if err := run(nil); err != nil {
		t.Fatal(err)
	}
	a, err := openAsset("")
	if err != nil {
		t.Fatal(err)
	}
	defer a.close()
	first, err := a.read(0)
	if err != nil || len(d.drawn) != 1 || d.drawn[0] != first || !d.full[0] || !d.closed {
		t.Fatal("normal launch did not draw bundled animation and clean up", err)
	}
	entries, err := os.ReadDir(".")
	if err != nil || len(entries) != 0 {
		t.Fatal("default launch extracted files", err)
	}
}

func TestExplicitMediaOverride(t *testing.T) {
	path := testAsset(t, 2, 2, 1)
	t.Setenv("C1_BADAPPLE_FILE", path)
	if defaultAsset() != path {
		t.Fatal("environment override ignored")
	}
	d := &fakeDisplay{keys: make(chan uint16, 1)}
	d.keys <- 16
	withDisplay(t, d)
	if err := run(nil); err != nil || d.drawn[0][0] != 1 {
		t.Fatal("environment animation not played", err)
	}
	t.Setenv("C1_BADAPPLE_FILE", "nonexistent.bap")
	d.drawn = nil
	d.keys <- 16
	if err := run([]string{"--file", path}); err != nil || d.drawn[0][0] != 1 {
		t.Fatal("explicit file did not override environment", err)
	}
}

func TestEmbeddedReaderRejectsInvalidData(t *testing.T) {
	for _, data := range []string{"", embeddedAnimation[:31], embeddedAnimation[:len(embeddedAnimation)-1], embeddedAnimation + "x"} {
		if _, err := parseAsset(strings.NewReader(data), int64(len(data))); err == nil {
			t.Fatal("invalid embedded data accepted")
		}
	}
}

func TestEmbeddedPlaybackCancellation(t *testing.T) {
	d := &fakeDisplay{keys: make(chan uint16)}
	withDisplay(t, d)
	ctx, cancel := context.WithCancel(context.Background())
	d.hook = cancel
	if err := play(ctx, options{interval: time.Second, loop: true}); err != nil || !d.closed {
		t.Fatal("embedded playback cancellation failed", err)
	}
}
