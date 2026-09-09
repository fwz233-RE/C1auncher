package main

import (
	"context"
	"encoding/csv"
	"errors"
	"os"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"
)

func TestKeyboard(t *testing.T) {
	s := state{interval: 700 * time.Millisecond}
	for key, want := range map[uint16]time.Duration{2: time.Second, 3: 700 * time.Millisecond, 4: 500 * time.Millisecond, 5: 300 * time.Millisecond, 6: 200 * time.Millisecond, 7: 100 * time.Millisecond} {
		s.key(key)
		if s.interval != want {
			t.Fatalf("key %d: %v", key, s.interval)
		}
	}
	s.interval = 700 * time.Millisecond
	if s.key(106) != newSegment || s.interval != 650*time.Millisecond {
		t.Fatal("right")
	}
	if s.key(12) != newSegment || s.interval != 700*time.Millisecond {
		t.Fatal("minus")
	}
	for i := 0; i < 100; i++ {
		s.key(13)
	}
	if s.interval != 100*time.Millisecond || s.key(106) != noAction {
		t.Fatal("lower clamp")
	}
	for i := 0; i < 100; i++ {
		s.key(105)
	}
	if s.interval != 2*time.Second || s.key(12) != noAction {
		t.Fatal("upper clamp")
	}
	for i := 0; i < 3; i++ {
		if s.key(50) != newSegment || s.mode != (i+1)%3 {
			t.Fatal("mode")
		}
	}
	if s.key(57) != showUI || !s.paused {
		t.Fatal("pause")
	}
	if s.key(28) != newSegment || s.paused {
		t.Fatal("resume")
	}
	if s.key(35) != showUI || !s.help || !s.paused {
		t.Fatal("help")
	}
	old := s.interval
	if s.key(7) != noAction || s.interval != old {
		t.Fatal("speed key while help open")
	}
	if s.key(35) != showUI || s.help || !s.paused {
		t.Fatal("close help must stay paused")
	}
	if s.key(46) != newSegment || !s.paused {
		t.Fatal("clean preserves pause")
	}
	for _, k := range []uint16{116, 143, 999} {
		if s.key(k) != noAction {
			t.Fatal("reserved key")
		}
	}
	for _, k := range []uint16{1, 45, 102, 158} {
		if s.key(k) != quit {
			t.Fatal("exit key")
		}
	}
}
func black(f frame, x, y int) bool { return f[(y/8)*width+x]&(0x80>>uint(y%8)) != 0 }
func TestFramePackingAndBarcode(t *testing.T) {
	var f frame
	pixel(&f, 0, 0, true)
	pixel(&f, 295, 151, true)
	pixel(&f, -1, 0, true)
	pixel(&f, 296, 152, true)
	if f[0] != 128 || f[frameBytes-1] != 1 {
		t.Fatal("packing")
	}
	for _, id := range []uint32{0, 1, 12345, 0xffffff} {
		f = render(state{interval: 700 * time.Millisecond}, id, 0, "RUN", "LOG OK")
		var got uint32
		for bit := 0; bit < 24; bit++ {
			x := 4 + bit*12 + 5
			a, b := black(f, x, 49), black(f, x, 59)
			if a == b {
				t.Fatal("not complementary")
			}
			got <<= 1
			if a {
				got |= 1
			}
		}
		if got != id {
			t.Fatalf("decoded %d want %d", got, id)
		}
		if !black(f, 8, 80) || black(f, 285, 80) {
			t.Fatal("reference patches")
		}
	}
	if helpFrame() == (frame{}) {
		t.Fatal("empty help")
	}
}
func TestPatterns(t *testing.T) {
	for mode := 0; mode < 3; mode++ {
		s := state{mode: mode}
		a := render(s, 1, 0, "RUN", "LOG OK")
		b := render(s, 2, time.Second, "RUN", "LOG OK")
		different := false
		for y := 68; y < 128; y++ {
			for x := 22; x < 272; x++ {
				if black(a, x, y) != black(b, x, y) {
					different = true
				}
			}
		}
		if !different {
			t.Fatalf("pattern %d did not move", mode)
		}
		if a != render(s, 1, 0, "RUN", "LOG OK") {
			t.Fatal("nondeterministic scene")
		}
	}
}

type fakeDisplay struct {
	keys   chan uint16
	frames []frame
	full   []bool
	hook   func(int) error
	closed bool
}

func (d *fakeDisplay) draw(f frame, full bool) error {
	d.frames = append(d.frames, f)
	d.full = append(d.full, full)
	if d.hook != nil {
		return d.hook(len(d.frames))
	}
	return nil
}
func (d *fakeDisplay) events() <-chan uint16 { return d.keys }
func (d *fakeDisplay) close() error          { d.closed = true; return nil }
func mockDisplay(t *testing.T, d *fakeDisplay) {
	t.Helper()
	old := createDisplay
	createDisplay = func() (display, error) { return d, nil }
	t.Cleanup(func() { createDisplay = old })
	t.Setenv("C1_C1ANCHER_TERMINAL", "")
}
func readLog(t *testing.T, path string) []map[string]string {
	t.Helper()
	f, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	rows, err := csv.NewReader(f).ReadAll()
	if err != nil {
		t.Fatal(err)
	}
	var result []map[string]string
	for _, r := range rows[1:] {
		m := map[string]string{}
		for i, k := range rows[0] {
			m[k] = r[i]
		}
		result = append(result, m)
	}
	return result
}
func TestLoopBusyAndPacing(t *testing.T) {
	l := openSessionLog(t.TempDir())
	d := &fakeDisplay{keys: make(chan uint16, 10)}
	mockDisplay(t, d)
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	d.hook = func(n int) error {
		if n == 2 {
			return syscall.EAGAIN
		}
		if n == 3 {
			time.Sleep(20 * time.Millisecond)
		}
		if n == 4 {
			cancel()
		}
		return nil
	}
	if err := playWithHold(ctx, state{interval: 10 * time.Millisecond}, l, time.Millisecond); err != nil {
		t.Fatal(err)
	}
	l.close()
	if !d.closed || len(d.frames) != 4 || !d.full[0] {
		t.Fatal("cleanup/full/count", len(d.frames))
	}
	for _, v := range d.full[1:] {
		if v {
			t.Fatal("automatic full during samples")
		}
	}
	var samples []map[string]string
	for _, r := range readLog(t, l.path) {
		if r["kind"] == "sample" {
			samples = append(samples, r)
		}
	}
	if len(samples) != 3 || samples[0]["status"] != "busy" || samples[1]["status"] != "accepted" {
		t.Fatal(samples)
	}
	prevEnd, _ := strconv.ParseInt(samples[1]["end_us"], 10, 64)
	nextStart, _ := strconv.ParseInt(samples[2]["start_us"], 10, 64)
	if nextStart-prevEnd < 10000 {
		t.Fatal("timer reset before draw returned")
	}
	for i, r := range samples {
		if r["id"] != strconv.Itoa(i+2) {
			t.Fatal("ID not unique across busy attempts")
		}
	}
}
func TestLoopControls(t *testing.T) {
	l := openSessionLog(t.TempDir())
	defer l.close()
	d := &fakeDisplay{keys: make(chan uint16, 10)}
	mockDisplay(t, d)
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	d.hook = func(n int) error {
		switch n {
		case 1:
			d.keys <- 19 // R: 300ms
		case 2:
			d.keys <- 57
		case 3:
			d.keys <- 35
		case 4:
			d.keys <- 35
		case 5:
			d.keys <- 28
		case 6:
			d.keys <- 45 // X: exit
		}
		return nil
	}
	if err := playWithHold(ctx, state{interval: 700 * time.Millisecond}, l, time.Millisecond); err != nil {
		t.Fatal(err)
	}
	if len(d.frames) != 6 || !d.full[0] || !d.full[1] || !d.full[5] {
		t.Fatal(d.full)
	}
	l.flush()
	segments := 0
	for _, r := range readLog(t, l.path) {
		if r["kind"] == "segment" {
			segments++
		}
	}
	if segments != 3 {
		t.Fatal("startup, speed change, resume must be separate segments", segments)
	}
}
func TestBusyClassification(t *testing.T) {
	if !busyError(syscall.EAGAIN) || !busyError(errors.Join(syscall.EAGAIN, syscall.EBUSY)) {
		t.Fatal("transient error not recognized")
	}
	if busyError(nil) || busyError(errors.Join(syscall.EAGAIN, errors.New("failed to restore fast refresh"))) {
		t.Fatal("fatal restoration error treated as busy")
	}
}

func TestFatalDisplayError(t *testing.T) {
	l := openSessionLog(t.TempDir())
	defer l.close()
	failure := errors.New("write failed")
	d := &fakeDisplay{keys: make(chan uint16), hook: func(int) error { return failure }}
	mockDisplay(t, d)
	if err := playWithHold(context.Background(), state{interval: time.Second}, l, 0); !errors.Is(err, failure) || !d.closed {
		t.Fatal(err)
	}
}
func TestLogFailure(t *testing.T) {
	l := openSessionLog(t.TempDir())
	if !l.healthy() {
		t.Fatal("log creation")
	}
	_ = l.file.Close()
	l.event("test", state{}, "closed file")
	if l.healthy() || l.label() != "NO LOG" {
		t.Fatal("failed log still healthy")
	}
	l.close()
	badPath := t.TempDir() + "/file"
	if err := os.WriteFile(badPath, []byte("x"), 0600); err != nil {
		t.Fatal(err)
	}
	bad := openSessionLog(badPath)
	if bad.healthy() {
		t.Fatal("file accepted as directory")
	}
	bad.close()
}
func TestFlagValidationWithoutHardware(t *testing.T) {
	old := createDisplay
	createDisplay = func() (display, error) { t.Fatal("opened hardware"); return nil, nil }
	defer func() { createDisplay = old }()
	for _, args := range [][]string{{"--version"}, {"--help"}} {
		if err := run(args); err != nil {
			t.Fatal(err)
		}
	}
	for _, line := range []string{"--interval 0", "--interval 3s", "--duration 0", "--duration 2h", "--pattern bad", "extra"} {
		if err := run(strings.Fields(line)); err == nil {
			t.Fatal("invalid flag accepted", line)
		}
	}
}
