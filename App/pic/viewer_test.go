package main

import (
	"bytes"
	"context"
	"errors"
	"image"
	"os"
	"path/filepath"
	"sync/atomic"
	"testing"
	"time"

	"c1device"
)

func TestRawFrameRoundTrip(t *testing.T) {
	var frame c1device.Frame
	for i := range frame {
		frame[i] = byte(i*73 + 19)
	}
	path := filepath.Join(t.TempDir(), "wallpaper.raw")
	if err := os.WriteFile(path, frame[:], 0644); err != nil {
		t.Fatal(err)
	}
	picture, err := loadPicture(path)
	if err != nil {
		t.Fatal(err)
	}
	if got := renderImmersive(picture); got != frame {
		t.Fatal("RAW bit order/orientation changed")
	}
	if err := os.WriteFile(path, frame[:len(frame)-1], 0644); err != nil {
		t.Fatal(err)
	}
	if _, err := loadPicture(path); err == nil {
		t.Fatal("truncated RAW accepted")
	}
}

func TestPictureRootsIncludeRawAndDeduplicate(t *testing.T) {
	root := t.TempDir()
	for _, dir := range []string{"Pic", "Pictures/album"} {
		if err := os.MkdirAll(filepath.Join(root, dir), 0755); err != nil {
			t.Fatal(err)
		}
	}
	for _, name := range []string{"Pic/wallpaper.raw", "Pictures/album/photo.JPG", "Pictures/transparent.PNG", "Pictures/ignore.txt"} {
		if err := os.WriteFile(filepath.Join(root, name), []byte("fixture"), 0644); err != nil {
			t.Fatal(err)
		}
	}
	pictures, err := scanPictureRoots([]string{filepath.Join(root, "Pic"), filepath.Join(root, "Pic"), filepath.Join(root, "missing"), filepath.Join(root, "Pictures")})
	if err != nil {
		t.Fatal(err)
	}
	if len(pictures) != 3 {
		t.Fatalf("got %d pictures: %v", len(pictures), pictures)
	}
}

func TestUnreadableRootDoesNotHideOtherPictures(t *testing.T) {
	root := t.TempDir()
	bad := filepath.Join(root, "not-directory")
	if err := os.WriteFile(bad, []byte("file"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "good.png"), []byte("fixture"), 0644); err != nil {
		t.Fatal(err)
	}
	pictures, err := scanPictureRoots([]string{bad, root})
	if err == nil || len(pictures) != 1 {
		t.Fatalf("pictures=%v err=%v", pictures, err)
	}
}

func TestWallpaperRequiresExplicitConfirmationAndMatchesImmersive(t *testing.T) {
	dir := t.TempDir()
	old := bytes.Repeat([]byte{0x55}, c1device.FrameBytes)
	path := filepath.Join(dir, "wallpaper.raw")
	if err := os.WriteFile(path, old, 0644); err != nil {
		t.Fatal(err)
	}
	picture := image.NewGray(image.Rect(0, 0, 200, 100))
	state := pictureState{current: picture, picturesDir: dir, immersive: true}
	state.handle(c1device.Event{Key: c1device.KeyPause})
	if !state.confirmWallpaper {
		t.Fatal("P did not prompt")
	}
	got, _ := os.ReadFile(path)
	if !bytes.Equal(got, old) {
		t.Fatal("P overwrote without confirmation")
	}
	state.handle(c1device.Event{Key: c1device.KeyBack})
	if state.confirmWallpaper || !state.immersive {
		t.Fatal("cancel left viewing mode")
	}
	state.handle(c1device.Event{Key: c1device.KeyPause})
	state.handle(c1device.Event{Key: c1device.KeyOK, Repeat: true})
	got, _ = os.ReadFile(path)
	if !bytes.Equal(got, old) {
		t.Fatal("repeated OK confirmed wallpaper")
	}
	state.handle(c1device.Event{Key: c1device.KeyOK})
	got, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	want := renderImmersive(picture)
	if !bytes.Equal(got, want[:]) {
		t.Fatal("saved wallpaper differs from clean full-screen frame")
	}
	entries, _ := os.ReadDir(dir)
	if len(entries) != 1 {
		t.Fatal("temporary wallpaper left behind")
	}
}

func TestWallpaperFailureAndLoadingDoNotOverwrite(t *testing.T) {
	dir := t.TempDir()
	if err := os.Mkdir(filepath.Join(dir, "wallpaper.raw"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := saveWallpaper(dir, c1device.Frame{}); err == nil {
		t.Fatal("directory target accepted")
	}
	state := pictureState{loading: true, current: image.NewGray(image.Rect(0, 0, 10, 10)), picturesDir: dir}
	state.handle(c1device.Event{Key: c1device.KeyPause})
	if state.confirmWallpaper {
		t.Fatal("loading image can be saved")
	}
}

func TestNavigationAndBackFromImmersive(t *testing.T) {
	state := pictureState{pictures: []Picture{{Path: "first"}, {Path: "second"}}, selected: 0, current: image.NewGray(image.Rect(0, 0, 10, 10))}
	state.handle(c1device.Event{Key: c1device.KeyOK})
	if !state.immersive {
		t.Fatal("OK did not enter full screen")
	}
	_, quit := state.handle(c1device.Event{Key: c1device.KeyBack})
	if quit || state.immersive {
		t.Fatal("back did not leave full screen first")
	}
	state.handle(c1device.Event{Key: c1device.KeyRight})
	if state.selected != 1 || !state.loadRequested || !state.loading || state.current != nil {
		t.Fatal("right did not request next image")
	}
	state.handle(c1device.Event{Key: c1device.KeyLeft})
	if state.selected != 0 {
		t.Fatal("left did not request previous image")
	}
}

func TestLoaderCancelsWithoutParallelDecodes(t *testing.T) {
	var active atomic.Int32
	var peak atomic.Int32
	started := make(chan string, 64)
	decode := func(ctx context.Context, path string) (image.Image, error) {
		n := active.Add(1)
		defer active.Add(-1)
		for old := peak.Load(); n > old && !peak.CompareAndSwap(old, n); old = peak.Load() {
		}
		started <- path
		if path != "last" {
			<-ctx.Done()
			return nil, ctx.Err()
		}
		return image.NewGray(image.Rect(0, 0, 1, 1)), nil
	}
	loader := newPictureLoader(context.Background(), decode)
	defer loader.close()
	loader.selectPath("first")
	select {
	case <-started:
	case <-time.After(2 * time.Second):
		t.Fatal("decode not started")
	}
	for i := 0; i < 20; i++ {
		loader.selectPath("superseded")
	}
	last := loader.selectPath("last")
	timer := time.NewTimer(3 * time.Second)
	defer timer.Stop()
	for {
		select {
		case result := <-loader.results:
			if result.generation != last {
				continue
			}
			if result.err != nil || result.picture == nil {
				t.Fatalf("latest request failed: %v", result.err)
			}
			if peak.Load() != 1 {
				t.Fatalf("parallel decodes=%d", peak.Load())
			}
			return
		case <-started:
		case <-timer.C:
			t.Fatal("latest request did not finish")
		}
	}
}

func TestCancelledPictureLoad(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	_, err := loadPictureContext(ctx, "nonexistent.png")
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("got %v", err)
	}
}

func TestExtremeAspectRatioRetainsOnePixel(t *testing.T) {
	for _, source := range []image.Rectangle{image.Rect(0, 0, 16000, 1), image.Rect(0, 0, 1, 16000)} {
		got := fitImageRect(source, image.Rect(0, 0, 296, 152))
		if got.Empty() || got.Dx() > 296 || got.Dy() > 152 {
			t.Fatalf("bad fit %v", got)
		}
	}
}
