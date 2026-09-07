package main

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestKeyMapAndRelease(t *testing.T) {
	m := newModel(defaultSong())
	now := time.Unix(100, 0)
	for i, c := range []uint16{30, 17, 31, 18, 32, 33, 20, 34, 21, 35, 22, 36, 37} {
		out, _ := m.handle(keyEvent{Code: c, Down: true}, now)
		if len(out) != 1 || out[0].MIDI != 60+i {
			t.Fatalf("note %d: %+v", i, out)
		}
		out, _ = m.handle(keyEvent{Code: c, Down: true}, now)
		if len(out) != 0 {
			t.Fatal("repeat retrigger")
		}
		out, _ = m.handle(keyEvent{Code: c}, now)
		if len(out) != 1 || out[0].Kind != "off" {
			t.Fatal("release missing")
		}
	}
	if len(m.Held) != 0 {
		t.Fatal("stuck keys")
	}
}
func TestOctaveChangeReleasesOriginalID(t *testing.T) {
	m := newModel(defaultSong())
	now := time.Now()
	m.handle(keyEvent{Code: 30, Down: true}, now)
	m.handle(keyEvent{Code: 45, Down: true}, now)
	out, _ := m.handle(keyEvent{Code: 30}, now)
	if len(out) != 1 || out[0].ID != 30 {
		t.Fatal(out)
	}
}
func TestLoopOverdubAndStop(t *testing.T) {
	m := newModel(defaultSong())
	now := time.Unix(100, 0)
	m.handle(keyEvent{Code: 19, Down: true}, now)
	if !m.Playing || !m.Recording {
		t.Fatal("record did not start")
	}
	m.handle(keyEvent{Code: 30, Down: true}, now.Add(m.stepDuration()))
	if len(m.Song.Pattern[1]) != 1 {
		t.Fatal("quantization")
	}
	m.handle(keyEvent{Code: 30}, now)
	m.handle(keyEvent{Code: 30, Down: true}, now.Add(m.stepDuration()))
	if len(m.Song.Pattern[1]) != 1 {
		t.Fatal("duplicate overdub")
	}
	out := m.tick(now.Add(m.stepDuration()))
	if len(out) != 2 || out[1].ID != 300 || out[1].MIDI != 60 {
		t.Fatal(out)
	}
	if out = m.tick(now.Add(m.stepDuration())); out != nil {
		t.Fatal("same step twice")
	}
	m.handle(keyEvent{Code: 57, Down: true}, now)
	if m.Recording || m.Playing {
		t.Fatal("stop")
	}
}
func TestClearRequiresSecondPress(t *testing.T) {
	m := newModel(demoSong())
	now := time.Now()
	m.handle(keyEvent{Code: 14, Down: true}, now)
	if m.noteCount() == 0 {
		t.Fatal("single key erased song")
	}
	m.handle(keyEvent{Code: 14, Down: true}, now.Add(time.Second))
	if m.noteCount() != 0 || !m.Dirty {
		t.Fatal("clear failed")
	}
}
func TestLimitsAndReset(t *testing.T) {
	m := newModel(defaultSong())
	now := time.Now()
	for i := 0; i < 50; i++ {
		for _, c := range []uint16{45, 106, 103, 15} {
			m.handle(keyEvent{Code: c, Down: true}, now)
		}
	}
	if e := m.Song.validate(); e != nil {
		t.Fatal(e)
	}
	m.handle(keyEvent{Code: 30, Down: true}, now)
	out, _ := m.handle(keyEvent{Reset: true}, now)
	if len(m.Held) != 0 || len(out) != 1 || out[0].Kind != "off-all" {
		t.Fatal("reset")
	}
}
func TestSongRoundtripAndCorruption(t *testing.T) {
	path := filepath.Join(t.TempDir(), "song.json")
	s, e := loadSong(path)
	if e != nil || s.BPM != 110 {
		t.Fatal(e)
	}
	s = demoSong()
	if e = saveSong(path, s); e != nil {
		t.Fatal(e)
	}
	if e = saveSong(path, s); e != nil {
		t.Fatal("replace:", e)
	}
	got, e := loadSong(path)
	if e != nil || len(got.Pattern[0]) != 2 {
		t.Fatal(e)
	}
	bad := []byte(`{"format":999}`)
	os.WriteFile(path, bad, 0600)
	if _, e = loadSong(path); e == nil {
		t.Fatal("accepted corrupt")
	}
	data, _ := os.ReadFile(path)
	if string(data) != string(bad) {
		t.Fatal("modified original")
	}
}
func TestInvalidSong(t *testing.T) {
	s := defaultSong()
	s.Pattern[0] = []Hit{{MIDI: 200}}
	if s.validate() == nil {
		t.Fatal("bad pitch")
	}
	s = defaultSong()
	s.Pattern[0] = make([]Hit, 9)
	if s.validate() == nil {
		t.Fatal("unbounded polyphony")
	}
}
func TestRenderAndPNG(t *testing.T) {
	m := newModel(defaultSong())
	now := time.Now()
	f := render(m, now)
	nonzero := 0
	for _, v := range f {
		if v != 0 {
			nonzero++
		}
	}
	if nonzero < 400 {
		t.Fatal("blank UI")
	}
	m.handle(keyEvent{Code: 30, Down: true}, now)
	if f == render(m, now) {
		t.Fatal("no key feedback")
	}
	m.Help = true
	if f == render(m, now) {
		t.Fatal("help unchanged")
	}
	path := filepath.Join(t.TempDir(), "preview.png")
	w, e := os.Create(path)
	if e != nil {
		t.Fatal(e)
	}
	if e = writePNG(w, f, 3); e != nil {
		t.Fatal(e)
	}
	w.Close()
}
func TestWAVExport(t *testing.T) {
	path := filepath.Join(t.TempDir(), "loop.wav")
	if e := exportWAV(path, demoSong()); e != nil {
		t.Fatal(e)
	}
	data, e := os.ReadFile(path)
	if e != nil || string(data[:4]) != "RIFF" || string(data[8:12]) != "WAVE" {
		t.Fatal(e)
	}
	if int(binary.LittleEndian.Uint32(data[40:44])) != len(data)-44 {
		t.Fatal("invalid data size")
	}
	if e = exportWAV(path, demoSong()); e == nil {
		t.Fatal("overwrote export")
	}
	silent := true
	for _, b := range data[44:] {
		if b != 0 {
			silent = false
			break
		}
	}
	if silent {
		t.Fatal("silent export")
	}
}
func TestVersionAndBadArgs(t *testing.T) {
	if err := run([]string{"--version"}); err != nil {
		t.Fatal(err)
	}
	if run([]string{"unknown"}) == nil {
		t.Fatal("unexpected argument")
	}
}
