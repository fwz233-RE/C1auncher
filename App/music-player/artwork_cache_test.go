package main

import (
	"bytes"
	"encoding/binary"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestLoadTrackCoverCreatesAndUsesCompactCache(t *testing.T) {
	directory := t.TempDir()
	trackPath := filepath.Join(directory, "track.mp3")
	writeArtworkTrack(t, trackPath, color.Gray{Y: 48})
	info, err := os.Stat(trackPath)
	if err != nil {
		t.Fatal(err)
	}
	first := requireGrayCover(t, loadTrackCover(trackPath))
	cacheInfo, err := os.Stat(artworkCachePath(trackPath))
	if err != nil {
		t.Fatal(err)
	}
	wantCacheSize := int64(artworkCacheHeaderSize + defaultCoverSize*defaultCoverSize)
	if cacheInfo.Size() != wantCacheSize {
		t.Fatalf("cache size = %d, want %d", cacheInfo.Size(), wantCacheSize)
	}

	if err := os.WriteFile(trackPath, make([]byte, info.Size()), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.Chtimes(trackPath, info.ModTime(), info.ModTime()); err != nil {
		t.Fatal(err)
	}
	second := requireGrayCover(t, loadTrackCover(trackPath))
	if !bytes.Equal(first.Pix, second.Pix) {
		t.Fatal("cache hit did not preserve compact artwork")
	}
}

func TestArtworkCacheInvalidatesWhenTrackChanges(t *testing.T) {
	directory := t.TempDir()
	trackPath := filepath.Join(directory, "track.mp3")
	writeArtworkTrack(t, trackPath, color.Gray{Y: 24})
	first := requireGrayCover(t, loadTrackCover(trackPath))

	writeArtworkTrack(t, trackPath, color.Gray{Y: 224})
	changed := time.Now().Add(2 * time.Second)
	if err := os.Chtimes(trackPath, changed, changed); err != nil {
		t.Fatal(err)
	}
	second := requireGrayCover(t, loadTrackCover(trackPath))
	if bytes.Equal(first.Pix, second.Pix) {
		t.Fatal("changed track reused stale artwork cache")
	}
	info, err := os.Stat(trackPath)
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := loadArtworkCache(trackPath, fingerprintArtwork(info)); !ok {
		t.Fatal("rebuilt artwork cache did not match changed track")
	}
}

func TestArtworkCacheRecoversFromCorruption(t *testing.T) {
	directory := t.TempDir()
	trackPath := filepath.Join(directory, "track.mp3")
	writeArtworkTrack(t, trackPath, color.Gray{Y: 96})
	first := requireGrayCover(t, loadTrackCover(trackPath))
	cachePath := artworkCachePath(trackPath)
	data, err := os.ReadFile(cachePath)
	if err != nil {
		t.Fatal(err)
	}
	data[len(data)-1] ^= 0xff
	if err := os.WriteFile(cachePath, data, 0o644); err != nil {
		t.Fatal(err)
	}
	second := requireGrayCover(t, loadTrackCover(trackPath))
	if !bytes.Equal(first.Pix, second.Pix) {
		t.Fatal("corrupt cache rebuild changed artwork")
	}
	info, err := os.Stat(trackPath)
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := loadArtworkCache(trackPath, fingerprintArtwork(info)); !ok {
		t.Fatal("corrupt artwork cache was not rebuilt")
	}
	matches, err := filepath.Glob(filepath.Join(directory, ".cover-cache-*.tmp"))
	if err != nil {
		t.Fatal(err)
	}
	if len(matches) != 0 {
		t.Fatalf("temporary cache files remain: %v", matches)
	}
}

func TestTrackWithoutArtworkRemovesStaleCache(t *testing.T) {
	trackPath := filepath.Join(t.TempDir(), "plain.mp3")
	writeArtworkTrack(t, trackPath, color.Gray{Y: 128})
	_ = requireGrayCover(t, loadTrackCover(trackPath))
	if _, err := os.Stat(artworkCachePath(trackPath)); err != nil {
		t.Fatalf("initial artwork cache was not created: %v", err)
	}
	if err := os.WriteFile(trackPath, []byte("not an ID3 track"), 0o644); err != nil {
		t.Fatal(err)
	}
	cover := requireGrayCover(t, loadTrackCover(trackPath))
	defaultPixels := requireGrayCover(t, defaultCover())
	if !bytes.Equal(cover.Pix, defaultPixels.Pix) {
		t.Fatal("track without artwork did not use default cover")
	}
	if _, err := os.Stat(artworkCachePath(trackPath)); !os.IsNotExist(err) {
		t.Fatalf("track without artwork retained stale cache: %v", err)
	}
}

func writeArtworkTrack(t *testing.T, path string, value color.Gray) {
	t.Helper()
	artwork := image.NewGray(image.Rect(0, 0, 64, 48))
	for index := range artwork.Pix {
		artwork.Pix[index] = value.Y
	}
	var encoded bytes.Buffer
	if err := png.Encode(&encoded, artwork); err != nil {
		t.Fatal(err)
	}
	body := append([]byte{0}, []byte("image/png")...)
	body = append(body, 0, 3, 0)
	body = append(body, encoded.Bytes()...)
	frame := make([]byte, 10+len(body))
	copy(frame, []byte("APIC"))
	binary.BigEndian.PutUint32(frame[4:8], uint32(len(body)))
	copy(frame[10:], body)
	tag := make([]byte, 10+len(frame)+128)
	copy(tag, []byte("ID3"))
	tag[3] = 3
	writeSyncSafe(tag[6:10], uint32(len(frame)))
	copy(tag[10:], frame)
	if err := os.WriteFile(path, tag, 0o644); err != nil {
		t.Fatal(err)
	}
}

func requireGrayCover(t *testing.T, cover image.Image) *image.Gray {
	t.Helper()
	gray, ok := cover.(*image.Gray)
	if !ok {
		t.Fatalf("cover type = %T, want *image.Gray", cover)
	}
	if gray.Bounds() != image.Rect(0, 0, defaultCoverSize, defaultCoverSize) {
		t.Fatalf("cover bounds = %v", gray.Bounds())
	}
	return gray
}
