package main

import (
	"bytes"
	"encoding/binary"
	"image"
	"image/color"
	"image/png"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"testing"
	"time"
)

func TestParseFFplayLines(t *testing.T) {
	_, duration, kind, ok := parseFFplayLine("  Duration: 01:02:03.50, start: 0.000000, bitrate: 192 kb/s")
	if !ok || kind != PlayerDuration || duration != time.Hour+2*time.Minute+3500*time.Millisecond {
		t.Fatalf("duration parse = %v, %v, %v", duration, kind, ok)
	}
	position, _, kind, ok := parseFFplayLine("  12.34 M-A:  0.000 fd=0 aq=0KB")
	if !ok || kind != PlayerProgress || position != 12340*time.Millisecond {
		t.Fatalf("stats parse = %v, %v, %v", position, kind, ok)
	}
	if _, _, _, ok := parseFFplayLine("metadata only"); ok {
		t.Fatal("unrelated output was parsed")
	}
}

func TestSplitCRLF(t *testing.T) {
	scanner := newTestScanner("one\rtwo\r\nthree\nfour")
	var values []string
	for scanner.Scan() {
		values = append(values, scanner.Text())
	}
	want := []string{"one", "two", "three", "four"}
	if strings.Join(values, ",") != strings.Join(want, ",") {
		t.Fatalf("tokens = %v, want %v", values, want)
	}
}

func newTestScanner(input string) *testScanner {
	return &testScanner{data: []byte(input)}
}

type testScanner struct {
	data []byte
	text string
}

func (scanner *testScanner) Scan() bool {
	if len(scanner.data) == 0 {
		return false
	}
	advance, token, _ := splitCRLF(scanner.data, true)
	if advance == 0 {
		return false
	}
	scanner.text = string(token)
	scanner.data = scanner.data[advance:]
	return true
}

func (scanner *testScanner) Text() string { return scanner.text }

func TestFormatPlaybackTime(t *testing.T) {
	cases := map[time.Duration]string{
		-1:               "00:00",
		0:                "00:00",
		65 * time.Second: "01:05",
		time.Hour + 2*time.Minute + 3*time.Second: "1:02:03",
	}
	for input, want := range cases {
		if got := formatPlaybackTime(input); got != want {
			t.Errorf("formatPlaybackTime(%v) = %q, want %q", input, got, want)
		}
	}
}

func TestLocateAndCompactID3Artwork(t *testing.T) {
	source := image.NewRGBA(image.Rect(0, 0, 320, 240))
	for y := 0; y < 240; y++ {
		for x := 0; x < 320; x++ {
			source.SetRGBA(x, y, color.RGBA{R: uint8(x), G: uint8(y), B: 96, A: 255})
		}
	}
	var encoded bytes.Buffer
	if err := png.Encode(&encoded, source); err != nil {
		t.Fatal(err)
	}
	body := append([]byte{0}, []byte("image/png")...)
	body = append(body, 0, 3, 0)
	imageStart := len(body)
	body = append(body, encoded.Bytes()...)
	frame := make([]byte, 10+len(body))
	copy(frame, []byte("APIC"))
	binary.BigEndian.PutUint32(frame[4:8], uint32(len(body)))
	copy(frame[10:], body)
	tag := make([]byte, 10+len(frame))
	copy(tag, []byte("ID3"))
	tag[3] = 3
	copy(tag[10:], frame)
	tagSize := uint32(len(frame))
	tag[6] = byte(tagSize >> 21)
	tag[7] = byte(tagSize >> 14)
	tag[8] = byte(tagSize >> 7)
	tag[9] = byte(tagSize)

	reader := bytes.NewReader(tag)
	location, ok := locateID3Artwork(reader)
	if !ok || location.offset != int64(20+imageStart) || location.size != int64(encoded.Len()) {
		t.Fatalf("location = %#v, ok = %v", location, ok)
	}
	cover, ok := decodeCompactCover(reader, location)
	if !ok || cover.Bounds() != image.Rect(0, 0, defaultCoverSize, defaultCoverSize) || len(cover.Pix) != defaultCoverSize*defaultCoverSize {
		t.Fatalf("compact cover = %#v, ok = %v", cover, ok)
	}
	if _, ok := locateID3Artwork(bytes.NewReader([]byte("not an mp3"))); ok {
		t.Fatal("non-ID3 data returned artwork")
	}
}

func TestArtworkLocatorUsesBoundedReads(t *testing.T) {
	body := make([]byte, maxArtworkMetadata+32*1024)
	copy(body, append([]byte{0}, []byte("image/png")...))
	body[len("image/png")+1] = 0
	body[len("image/png")+2] = 3
	body[len("image/png")+3] = 0
	copy(body[len("image/png")+4:], []byte("\x89PNG\r\n\x1a\n"))
	frame := make([]byte, 10+len(body))
	copy(frame, []byte("APIC"))
	binary.BigEndian.PutUint32(frame[4:8], uint32(len(body)))
	copy(frame[10:], body)
	tag := make([]byte, 10+len(frame))
	copy(tag, []byte("ID3"))
	tag[3] = 3
	writeSyncSafe(tag[6:10], uint32(len(frame)))
	copy(tag[10:], frame)

	reader := &trackingReaderAt{reader: bytes.NewReader(tag)}
	location, ok := locateID3Artwork(reader)
	if !ok || location.size != int64(len(body)-len("image/png")-4) {
		t.Fatalf("location = %#v, ok = %v", location, ok)
	}
	if reader.maxRead > maxArtworkMetadata {
		t.Fatalf("largest metadata read = %d, limit = %d", reader.maxRead, maxArtworkMetadata)
	}
	if reader.totalRead >= len(tag) {
		t.Fatalf("locator read %d bytes from %d-byte tag", reader.totalRead, len(tag))
	}
}

func TestRealArtworkSampleRetainsOnlyCompactCover(t *testing.T) {
	sourcePath := os.Getenv("C1_ARTWORK_SAMPLE")
	if sourcePath == "" {
		t.Skip("C1_ARTWORK_SAMPLE is not set")
	}
	path := filepath.Join(t.TempDir(), "sample.mp3")
	source, err := os.Open(sourcePath)
	if err != nil {
		t.Fatal(err)
	}
	destination, err := os.Create(path)
	if err != nil {
		source.Close()
		t.Fatal(err)
	}
	_, copyErr := io.Copy(destination, source)
	closeDestinationErr := destination.Close()
	closeSourceErr := source.Close()
	if copyErr != nil {
		t.Fatal(copyErr)
	}
	if closeDestinationErr != nil {
		t.Fatal(closeDestinationErr)
	}
	if closeSourceErr != nil {
		t.Fatal(closeSourceErr)
	}

	file, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	_, ok := locateID3Artwork(file)
	if !ok {
		file.Close()
		t.Fatal("sample artwork was not located")
	}
	file.Close()

	runtime.GC()
	var before, after runtime.MemStats
	runtime.ReadMemStats(&before)
	cover := loadTrackCover(path)
	runtime.GC()
	runtime.ReadMemStats(&after)
	if cover.Bounds() != image.Rect(0, 0, defaultCoverSize, defaultCoverSize) {
		t.Fatalf("cover bounds = %v", cover.Bounds())
	}
	if _, ok := cover.(*image.Gray); !ok {
		t.Fatalf("cover type = %T, want *image.Gray", cover)
	}
	if after.HeapAlloc > before.HeapAlloc+2<<20 {
		t.Fatalf("retained heap grew by %d bytes", after.HeapAlloc-before.HeapAlloc)
	}
	if _, err := os.Stat(artworkCachePath(path)); err != nil {
		t.Fatalf("real artwork cache was not created: %v", err)
	}
	cached := loadTrackCover(path)
	if !bytes.Equal(cover.(*image.Gray).Pix, cached.(*image.Gray).Pix) {
		t.Fatal("real artwork cache hit changed compact cover")
	}
	runtime.KeepAlive(cover)
}

func writeSyncSafe(target []byte, value uint32) {
	target[0] = byte(value >> 21)
	target[1] = byte(value >> 14)
	target[2] = byte(value >> 7)
	target[3] = byte(value)
}

type trackingReaderAt struct {
	reader    io.ReaderAt
	maxRead   int
	totalRead int
}

func (reader *trackingReaderAt) ReadAt(target []byte, offset int64) (int, error) {
	if len(target) > reader.maxRead {
		reader.maxRead = len(target)
	}
	read, err := reader.reader.ReadAt(target, offset)
	reader.totalRead += read
	return read, err
}

func TestDefaultCoverIsDeviceSized(t *testing.T) {
	cover := defaultCover()
	if got := cover.Bounds().Dx(); got != defaultCoverSize {
		t.Fatalf("cover width = %d, want %d", got, defaultCoverSize)
	}
	if got := cover.Bounds().Dy(); got != defaultCoverSize {
		t.Fatalf("cover height = %d, want %d", got, defaultCoverSize)
	}
	gray := color.GrayModel.Convert(cover.At(45, 47)).(color.Gray)
	label := color.GrayModel.Convert(cover.At(35, 47)).(color.Gray)
	field := color.GrayModel.Convert(cover.At(70, 47)).(color.Gray)
	ring := color.GrayModel.Convert(cover.At(6, 47)).(color.Gray)
	if gray.Y != 255 || label.Y != 0 || field.Y != 255 || ring.Y != 0 {
		t.Fatalf("default cover layers = center:%d label:%d field:%d ring:%d", gray.Y, label.Y, field.Y, ring.Y)
	}
}

func TestFFplayDisablesAttachedPictures(t *testing.T) {
	arguments := ffplayArguments("track.mp3")
	if !containsString(arguments, "-vn") {
		t.Fatalf("ffplay arguments do not disable video: %v", arguments)
	}
	if arguments[len(arguments)-2] != "--" || arguments[len(arguments)-1] != "track.mp3" {
		t.Fatalf("ffplay path arguments = %v", arguments)
	}
}

func containsString(values []string, target string) bool {
	for _, value := range values {
		if value == target {
			return true
		}
	}
	return false
}

func TestPlayerFakeStateAndWaitOnce(t *testing.T) {
	fake := newFakeProcess()
	player := newPlayer(func(string) (process, error) { return fake, nil })
	player.grace = 20 * time.Millisecond
	if err := player.Play("track.mp3"); err != nil {
		t.Fatal(err)
	}
	generation := player.Generation()
	if generation == 0 {
		t.Fatal("generation was not assigned")
	}
	if err := player.PauseToggle(); err != nil {
		t.Fatal(err)
	}
	if err := player.PauseToggle(); err != nil {
		t.Fatal(err)
	}
	player.Stop()

	fake.mu.Lock()
	defer fake.mu.Unlock()
	if !fake.started {
		t.Fatal("process was not started")
	}
	if fake.waitCalls != 1 {
		t.Fatalf("Wait calls = %d, want 1", fake.waitCalls)
	}
	if len(fake.signals) < 4 {
		t.Fatalf("signals = %v, expected pause, continue, stop continue and terminate", fake.signals)
	}
	if player.Generation() == generation {
		t.Fatal("stop did not invalidate generation")
	}
	if eventApplies(player.Generation(), PlayerEvent{Generation: generation}) {
		t.Fatal("stale event applies to new generation")
	}
}

type fakeProcess struct {
	mu        sync.Mutex
	started   bool
	signals   []os.Signal
	waitCalls int
	wait      chan struct{}
	closeOnce sync.Once
}

func newFakeProcess() *fakeProcess { return &fakeProcess{wait: make(chan struct{})} }
func (fake *fakeProcess) Start() error {
	fake.mu.Lock()
	fake.started = true
	fake.mu.Unlock()
	return nil
}
func (fake *fakeProcess) Stderr() io.ReadCloser { return io.NopCloser(strings.NewReader("")) }
func (fake *fakeProcess) Signal(signal os.Signal) error {
	fake.mu.Lock()
	fake.signals = append(fake.signals, signal)
	fake.mu.Unlock()
	if signal.String() == processSignalTerminate.String() {
		fake.closeOnce.Do(func() { close(fake.wait) })
	}
	return nil
}
func (fake *fakeProcess) Kill() error {
	fake.closeOnce.Do(func() { close(fake.wait) })
	return nil
}
func (fake *fakeProcess) Wait() error {
	fake.mu.Lock()
	fake.waitCalls++
	fake.mu.Unlock()
	<-fake.wait
	return nil
}
