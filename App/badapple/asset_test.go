package main

import (
	"context"
	"encoding/binary"
	"errors"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func testAsset(t *testing.T, count uint32, num, den uint32) string {
	t.Helper()
	data := make([]byte, headerBytes+int(count)*frameBytes)
	copy(data, assetMagic)
	binary.LittleEndian.PutUint16(data[8:], width)
	binary.LittleEndian.PutUint16(data[10:], height)
	for off, v := range map[int]uint32{12: num, 16: den, 20: count, 24: frameBytes} {
		binary.LittleEndian.PutUint32(data[off:], v)
	}
	for i := uint32(0); i < count; i++ {
		data[headerBytes+int(i)*frameBytes] = byte(i + 1)
	}
	path := filepath.Join(t.TempDir(), "test.bap")
	if err := os.WriteFile(path, data, 0600); err != nil {
		t.Fatal(err)
	}
	return path
}
func TestAssetReadAndTiming(t *testing.T) {
	path := testAsset(t, 10, 2, 1)
	a, err := openAsset(path)
	if err != nil {
		t.Fatal(err)
	}
	defer a.close()
	if a.duration() != 5*time.Second {
		t.Fatal(a.duration())
	}
	for _, tc := range []struct {
		at    time.Duration
		index uint32
	}{{-1, 0}, {0, 0}, {499 * time.Millisecond, 0}, {500 * time.Millisecond, 1}, {time.Second, 2}, {20 * time.Second, 9}} {
		if got := a.index(tc.at); got != tc.index {
			t.Fatalf("index(%v)=%d", tc.at, got)
		}
	}
	f, err := a.read(8)
	if err != nil || f[0] != 9 {
		t.Fatal(f[0], err)
	}
	if _, err = a.read(10); err == nil {
		t.Fatal("out of range accepted")
	}
}
func TestRationalTiming(t *testing.T) {
	a, err := openAsset(testAsset(t, 300, 30000, 1001))
	if err == nil {
		a.close()
		t.Fatal("denominator limit accepted")
	}
	a, err = openAsset(testAsset(t, 300, 2997, 100))
	if err != nil {
		t.Fatal(err)
	}
	defer a.close()
	for ms := 0; ms < 11000; ms++ {
		want := uint32(int64(ms) * 2997 / 100000)
		if want >= a.count {
			want = a.count - 1
		}
		if got := a.index(time.Duration(ms) * time.Millisecond); got != want {
			t.Fatalf("ms %d got %d want %d", ms, got, want)
		}
	}
}
func TestRejectMalformedAssets(t *testing.T) {
	for _, tc := range []struct {
		name string
		edit func([]byte) []byte
	}{
		{"short", func(b []byte) []byte { return b[:10] }},
		{"truncated", func(b []byte) []byte { return b[:len(b)-1] }},
		{"extra", func(b []byte) []byte { return append(b, 0) }},
		{"magic", func(b []byte) []byte { b[0] = 0; return b }},
		{"width", func(b []byte) []byte { b[8] = 0; return b }},
		{"reserved", func(b []byte) []byte { b[28] = 1; return b }},
		{"zero-fps", func(b []byte) []byte { clear(b[12:16]); return b }},
		{"zero-den", func(b []byte) []byte { clear(b[16:20]); return b }},
		{"zero-count", func(b []byte) []byte { clear(b[20:24]); return b }},
		{"huge-count", func(b []byte) []byte { binary.LittleEndian.PutUint32(b[20:], 0xffffffff); return b }},
		{"fast", func(b []byte) []byte { binary.LittleEndian.PutUint32(b[12:], 31); return b }},
		{"frame-size", func(b []byte) []byte { binary.LittleEndian.PutUint32(b[24:], 1); return b }},
	} {
		t.Run(tc.name, func(t *testing.T) {
			path := testAsset(t, 2, 2, 1)
			b, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			if err = os.WriteFile(path, tc.edit(b), 0600); err != nil {
				t.Fatal(err)
			}
			a, err := openAsset(path)
			if err == nil {
				a.close()
				t.Fatal("malformed asset accepted")
			}
		})
	}
}
func TestPlaybackClock(t *testing.T) {
	now := time.Unix(100, 0)
	p := playback{started: now, length: 10 * time.Second, loop: true}
	if p.position(now.Add(23*time.Second)) != 3*time.Second {
		t.Fatal("loop")
	}
	p.toggle(now.Add(3 * time.Second))
	if !p.paused || p.position(now.Add(time.Hour)) != 3*time.Second {
		t.Fatal("pause")
	}
	p.toggle(now.Add(10 * time.Second))
	if p.position(now.Add(11*time.Second)) != 4*time.Second {
		t.Fatal("resume")
	}
	p.seek(-20*time.Second, now.Add(11*time.Second))
	if p.position(now.Add(11*time.Second)) != 0 {
		t.Fatal("seek start")
	}
	p.restart(now.Add(12 * time.Second))
	if p.position(now.Add(13*time.Second)) != time.Second {
		t.Fatal("restart")
	}
	p.loop = false
	if p.position(now.Add(time.Hour)) != 10*time.Second {
		t.Fatal("end")
	}
}
func TestPixelsAndUI(t *testing.T) {
	var f frame
	pixel(&f, 0, 0, true)
	pixel(&f, 295, 151, true)
	pixel(&f, 296, 0, true)
	pixel(&f, -1, 0, true)
	if f[0] != 128 || f[frameBytes-1] != 1 {
		t.Fatal("packing")
	}
	pixel(&f, 0, 0, false)
	if f[0] != 0 {
		t.Fatal("clear")
	}
	if messageFrame("BAD APPLE") == (frame{}) {
		t.Fatal("empty UI")
	}
	if exitKey(143) || pauseKey(143) || exitKey(116) || pauseKey(116) {
		t.Fatal("power key consumed")
	}
}

type fakeDisplay struct {
	keys   chan uint16
	drawn  []frame
	full   []bool
	closed bool
	err    error
	hook   func()
}

func (d *fakeDisplay) draw(f frame, full bool) error {
	d.drawn = append(d.drawn, f)
	d.full = append(d.full, full)
	if d.hook != nil {
		d.hook()
	}
	return d.err
}
func (d *fakeDisplay) events() <-chan uint16 { return d.keys }
func (d *fakeDisplay) close() error          { d.closed = true; return nil }
func withDisplay(t *testing.T, d *fakeDisplay) {
	t.Helper()
	old := createDisplay
	createDisplay = func() (display, error) { return d, nil }
	t.Cleanup(func() { createDisplay = old })
	t.Setenv("C1_C1ANCHER_TERMINAL", "")
}
func TestDeviceLoopControlsAndCleanup(t *testing.T) {
	d := &fakeDisplay{keys: make(chan uint16, 8)}
	withDisplay(t, d)
	for _, k := range []uint16{143, 57, 19, 57, 35, 35, 102} {
		d.keys <- k
	}
	err := play(context.Background(), options{path: testAsset(t, 10, 2, 1), interval: time.Second, loop: true})
	if err != nil || !d.closed || len(d.drawn) < 4 || !d.full[0] {
		t.Fatal(err, d.closed, len(d.drawn))
	}
	fulls := 0
	for _, v := range d.full {
		if v {
			fulls++
		}
	}
	if fulls != 2 {
		t.Fatalf("expected entry + manual full: %v", d.full)
	}
}
func TestDeviceLoopFailures(t *testing.T) {
	for _, badMedia := range []bool{false, true} {
		t.Run(map[bool]string{false: "draw-error", true: "missing-media"}[badMedia], func(t *testing.T) {
			d := &fakeDisplay{keys: make(chan uint16, 1)}
			withDisplay(t, d)
			path := testAsset(t, 2, 2, 1)
			if badMedia {
				path = filepath.Join(t.TempDir(), "absent.bap")
				d.keys <- 16
			} else {
				d.err = errors.New("screen failed")
			}
			err := play(context.Background(), options{path: path, interval: time.Second})
			if !badMedia && !errors.Is(err, d.err) {
				t.Fatal(err)
			}
			if !d.closed {
				t.Fatal("leaked display")
			}
		})
	}
}
func TestMetadataNeverOpensHardware(t *testing.T) {
	old := createDisplay
	createDisplay = func() (display, error) { t.Fatal("hardware opened"); return nil, nil }
	defer func() { createDisplay = old }()
	for _, args := range [][]string{{"--version"}, {"--help"}, {"--check", testAsset(t, 2, 2, 1)}} {
		if err := run(args); err != nil {
			t.Fatal(err)
		}
	}
}
func TestReadDetectsPostOpenTruncation(t *testing.T) {
	path := testAsset(t, 2, 2, 1)
	a, err := openAsset(path)
	if err != nil {
		t.Fatal(err)
	}
	defer a.close()
	if err = os.Truncate(path, headerBytes+frameBytes); err != nil {
		t.Fatal(err)
	}
	if _, err = a.read(1); err == nil {
		t.Fatal("short read accepted")
	}
}
