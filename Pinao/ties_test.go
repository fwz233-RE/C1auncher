package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"os"
	"path/filepath"
	"reflect"
	"testing"
	"time"
)

func tieModel() (*model, time.Time) {
	m := newModel(defaultSong())
	m.StepMode, m.Recording, m.Step = true, true, 0
	return m, time.Unix(100, 0)
}
func TestTieHeldChordReleaseAndTone(t *testing.T) {
	m, now := tieModel()
	press(m, 30, now)
	press(m, 32, now)
	press(m, 46, now)
	press(m, 16, now)
	press(m, 45, now) // Original held pitch and tone must be retained.
	press(m, 106, now)
	want := []Hit{{MIDI: 60, Tie: true}, {MIDI: 64, Tie: true}}
	if !reflect.DeepEqual(m.Song.Pattern[1], want) {
		t.Fatal(m.Song.Pattern[1])
	}
	m.handle(keyEvent{Code: 30}, now)
	press(m, 106, now)
	if !reflect.DeepEqual(m.Song.Pattern[2], want[1:]) {
		t.Fatal("released note or drum extended")
	}
	m.handle(keyEvent{Code: 32}, now)
	press(m, 106, now)
	if len(m.Song.Pattern[3]) != 0 {
		t.Fatal("release created an event")
	}
	if err := m.Song.validate(); err != nil {
		t.Fatal(err)
	}
	if m.Song.Format != 3 || !m.Dirty {
		t.Fatal("format/dirty")
	}
}
func TestTieNavigationSafety(t *testing.T) {
	for _, action := range []uint16{105, 19, 38} {
		m, now := tieModel()
		press(m, 30, now)
		press(m, action, now)
		if action == 38 {
			press(m, 38, now)
		}
		press(m, 106, now)
		if m.Song.Format != 1 {
			t.Fatalf("action %d left armed capture", action)
		}
	}
	m, now := tieModel()
	m.Step = 15
	press(m, 30, now)
	press(m, 106, now)
	if m.Step != 0 || len(m.Song.Pattern[0]) != 0 {
		t.Fatal("wrap created backwards tie")
	}
	m, now = tieModel()
	press(m, 30, now)
	m.handle(keyEvent{Reset: true}, now)
	press(m, 106, now)
	if len(m.Song.Pattern[1]) != 0 {
		t.Fatal("input reset extended held note")
	}
	m, now = tieModel()
	m.Recording = false
	press(m, 30, now)
	press(m, 106, now)
	if m.noteCount() != 0 {
		t.Fatal("browse wrote music")
	}
	m, now = tieModel()
	m.StepMode = false
	m.start(now)
	press(m, 30, now)
	press(m, 106, now)
	if m.noteCount() != 1 {
		t.Fatal("auto to manual unexpectedly extended")
	}
}
func TestTieFullCellAndExistingAttack(t *testing.T) {
	m, now := tieModel()
	press(m, 30, now)
	for i := 0; i < 8; i++ {
		m.Song.Pattern[1] = append(m.Song.Pattern[1], Hit{MIDI: 65 + i})
	}
	before := mustJSONSong(t, m.Song)
	press(m, 106, now)
	if !bytes.Equal(before, mustJSONSong(t, m.Song)) {
		t.Fatal("full cell overwrote existing notes")
	}
	press(m, 106, now)
	if len(m.Song.Pattern[2]) != 0 {
		t.Fatal("tie jumped a full cell")
	}
	m, now = tieModel()
	m.Song.Pattern[1] = []Hit{{MIDI: 60}}
	press(m, 30, now)
	press(m, 106, now)
	press(m, 106, now)
	if m.Song.Pattern[1][0].Tie || len(m.Song.Pattern[2]) != 0 {
		t.Fatal("existing attack overwritten")
	}
}
func TestTiePagesDeletionAndRetrigger(t *testing.T) {
	m, now := tieModel()
	m.Step = 15
	press(m, 30, now)
	m.handle(keyEvent{Code: 24, Down: true}, now)
	m.tick(now.Add(pageHoldDuration))
	m.handle(keyEvent{Code: 24}, now.Add(pageHoldDuration))
	if m.Page != 1 || !m.Song.Pages[0][0][0].Tie || m.Song.Format != 3 {
		t.Fatal("insert page did not extend last cell")
	}
	press(m, 106, now)
	m.handle(keyEvent{Code: 30}, now)
	if err := m.Song.validate(); err != nil {
		t.Fatal(err)
	}
	m.Page, m.Step = 0, 15
	if !m.view(now).TieOut {
		t.Fatal("missing outgoing line")
	}
	press(m, 111, now)
	if m.Song.Pages[0][0][0].Tie || !m.Song.Pages[0][1][0].Tie {
		t.Fatal("delete did not repair continuation")
	}
	m.Page, m.Step = 1, 1
	press(m, 30, now)
	if m.Song.Pages[0][1][0].Tie || len(m.Song.Pages[0][1]) != 1 {
		t.Fatal("new key press did not rearticulate")
	}
	m.handle(keyEvent{Code: 30}, now)
	m.Page, m.Step = 0, 0
	m.insertPage(1, now)
	if m.Song.Format != 3 {
		t.Fatal("insert downgraded format")
	}
	if err := m.Song.validate(); err != nil {
		t.Fatal(err)
	}
}
func TestTiePlaybackPreservesVoiceAndStopsOnRest(t *testing.T) {
	m, now := tieModel()
	press(m, 30, now)
	press(m, 106, now)
	press(m, 106, now)
	m.handle(keyEvent{Code: 30}, now)
	m.Recording = false
	m.start(now)
	s := NewSynth()
	for _, c := range m.tick(now) {
		applySound(s, c)
	}
	renderFrames(s, 1000)
	serial := s.serial
	phase := s.voices[0].phase
	for _, c := range m.tick(now.Add(m.stepDuration())) {
		applySound(s, c)
	}
	if s.serial != serial || s.voices[0].releasing || s.voices[0].phase != phase {
		t.Fatal("tie retriggered/released")
	}
	for _, c := range m.tick(now.Add(2 * m.stepDuration())) {
		applySound(s, c)
	}
	if s.serial != serial {
		t.Fatal("second continuation retriggered")
	}
	for _, c := range m.tick(now.Add(3 * m.stepDuration())) {
		applySound(s, c)
	}
	if !s.voices[0].releasing {
		t.Fatal("rest did not release")
	}
	// Starting in a continuation or skipping ticks must sound it from scratch.
	m.startFrom(now, 1)
	if out := m.tick(now); out[0].Keep != 0 || out[1].Kind != "on" {
		t.Fatal(out)
	}
	m.start(now)
	m.tick(now)
	if out := m.tick(now.Add(2 * m.stepDuration())); out[0].Keep != 0 || out[1].Kind != "on" {
		t.Fatal(out)
	}
}
func TestTieStableSlotsAndTempo(t *testing.T) {
	var p loopTransport
	var buf [9]soundCommand
	p.commands(buf[:0], []Hit{{MIDI: 60}, {MIDI: 64}}, 0, true)
	out := p.commands(buf[:0], []Hit{{Drum: 1}, {MIDI: 64, Tie: true}, {MIDI: 60, Tie: true}}, 1, true)
	if out[0].Keep != 3 || out[1].ID != 302 || out[2].ID != 301 || out[3].ID != 300 {
		t.Fatal(out)
	}
	out = p.commands(buf[:0], []Hit{{MIDI: 60}}, 0, true)
	if out[0].Keep != 0 || out[1].Kind != "on" {
		t.Fatal("song wrap tied")
	}
	m, now := tieModel()
	press(m, 30, now)
	press(m, 106, now)
	m.start(now)
	m.tick(now)
	press(m, 103, now.Add(m.stepDuration()/2))
	out = m.tick(m.Started.Add(m.stepDuration()))
	if out[0].Keep != 1 || out[1].Kind != "hold" {
		t.Fatal("tempo retriggered tie", out)
	}
}
func TestTieHoldTimeoutAndStolenVoice(t *testing.T) {
	s := NewSynth()
	s.NoteOn(300, 60, 0, 0.8)
	v := &s.voices[0]
	v.age = maxHoldSamples - 10
	s.Hold(300, 60, 0)
	renderFrames(s, 100)
	if v.releasing {
		t.Fatal("tie expired at manual key timeout")
	}
	s.NoteOn(300, 65, 1, 0.8)
	s.Hold(300, 60, 0)
	if s.voices[0].midi != 60 || s.voices[0].timbre != 0 {
		t.Fatal("wrong note kept alive")
	}
}
func TestTieStorageMigrationAndValidation(t *testing.T) {
	for _, pages := range []int{1, 2} {
		path := filepath.Join(t.TempDir(), "song.json")
		old := mpSong(pages)
		if err := saveSong(path, old); err != nil {
			t.Fatal(err)
		}
		original := mpRead(t, path)
		s := old.clone()
		s.Format = 3
		s.Pattern[0] = []Hit{{MIDI: 60}}
		s.Pattern[1] = []Hit{{MIDI: 60, Tie: true}}
		if err := saveSong(path, s); err != nil {
			t.Fatal(err)
		}
		h := sha256.Sum256(original)
		if !bytes.Equal(original, mpRead(t, path+".pre-ties-"+hex.EncodeToString(h[:])+".json")) {
			t.Fatal("backup differs")
		}
		got, err := loadSong(path)
		if err != nil || !bytes.Equal(mustJSONSong(t, got), mustJSONSong(t, s)) {
			t.Fatal("roundtrip", err)
		}
		if err := saveSong(path, s); err != nil {
			t.Fatal("repeat save", err)
		}
	}
	s := defaultSong()
	s.Format = 3
	s.Pattern[0] = []Hit{{MIDI: 60, Tie: true}}
	if s.validate() == nil {
		t.Fatal("first-cell tie accepted")
	}
	s.Pattern[0] = []Hit{{MIDI: 60}}
	s.Pattern[1] = []Hit{{MIDI: 61, Tie: true}}
	if s.validate() == nil {
		t.Fatal("orphan accepted")
	}
	s.Pattern[1] = []Hit{{Drum: 1, Tie: true}}
	if s.validate() == nil {
		t.Fatal("drum tie accepted")
	}
	s.Pattern[1] = []Hit{{MIDI: 60, Tie: true}}
	s.Format = 2
	if s.validate() == nil {
		t.Fatal("legacy tie accepted")
	}
}
func TestTieExportMatchesSustainedReference(t *testing.T) {
	song := defaultSong()
	song.BPM = 120
	song.Format = 3
	song.Pattern[0] = []Hit{{MIDI: 60}}
	song.Pattern[1] = []Hit{{MIDI: 60, Tie: true}}
	song.Pattern[2] = []Hit{{MIDI: 60, Tie: true}}
	path := filepath.Join(t.TempDir(), "tie.wav")
	if err := exportWAV(path, song); err != nil {
		t.Fatal(err)
	}
	wav := mpRead(t, path)
	s := NewSynth()
	s.SetVolume(song.Volume)
	s.NoteOn(300, 60, 0, 0.8)
	held := renderFrames(s, 3*sampleRate/4)
	s.NoteOff(300)
	pcm := append(held, renderFrames(s, (steps-3)*sampleRate/4+sampleRate)...)
	raw := make([]byte, len(pcm)*2)
	encodePCM(raw, pcm)
	if !bytes.Equal(wav[44:], raw) {
		t.Fatal("WAV shortened or retriggered held note")
	}
	if binary.LittleEndian.Uint32(wav[40:44]) != uint32(len(raw)) {
		t.Fatal("header size")
	}
}
func TestTieRenderingAndAllocation(t *testing.T) {
	m, now := tieModel()
	press(m, 30, now)
	press(m, 106, now)
	v := m.view(now)
	if v.Ties != 2 {
		t.Fatal(v.Ties)
	}
	f := renderView(v)
	v.Ties = 0
	plain := renderView(v)
	if f == plain {
		t.Fatal("line missing")
	}
	for x := 20; x < 25; x++ {
		if f[(126/8)*width+x]&(0x80>>uint(126&7)) == 0 {
			t.Fatal("gap in line")
		}
	}
	alloc := testing.AllocsPerRun(100, func() { m.start(now); m.tick(now); m.tick(now.Add(m.stepDuration())) })
	if alloc != 0 {
		t.Fatal("tie tick allocated", alloc)
	}
	if dir := os.Getenv("PINAO_TEST_PREVIEW_DIR"); dir != "" {
		file, err := os.Create(filepath.Join(dir, "ties.png"))
		if err != nil {
			t.Fatal(err)
		}
		err = writePNG(file, f, 3)
		closeErr := file.Close()
		if err != nil || closeErr != nil {
			t.Fatal(err, closeErr)
		}
	}
}
