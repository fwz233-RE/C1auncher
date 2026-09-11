package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestTieMigrationFailurePreservesOriginal(t *testing.T) {
	path := filepath.Join(t.TempDir(), "song.json")
	s := mpSong(2)
	if err := saveSong(path, s); err != nil {
		t.Fatal(err)
	}
	before := mpRead(t, path)
	h := sha256.Sum256(before)
	collision := path + ".pre-ties-" + hex.EncodeToString(h[:]) + ".json"
	if err := os.WriteFile(collision, []byte("conflict"), 0600); err != nil {
		t.Fatal(err)
	}
	s.Format = 3
	s.Pattern[0] = []Hit{{MIDI: 60}}
	s.Pattern[1] = []Hit{{MIDI: 60, Tie: true}}
	if err := saveSong(path, s); err == nil {
		t.Fatal("backup conflict allowed overwrite")
	}
	if !bytes.Equal(before, mpRead(t, path)) {
		t.Fatal("original changed")
	}
}

func TestTieExistingPageAndInsertionBreaksOldBridge(t *testing.T) {
	m, now := tieModel()
	m.Song.Pages = make([][steps][]Hit, 1)
	m.Song.Format = 2
	m.Step = 15
	press(m, 30, now)
	m.handle(keyEvent{Code: 24, Down: true}, now)
	m.handle(keyEvent{Code: 24}, now.Add(time.Millisecond))
	if m.Page != 1 || !m.Song.Pages[0][0][0].Tie {
		t.Fatal("tap page lost held note")
	}
	m.handle(keyEvent{Code: 30}, now)
	m.Page, m.Step = 0, 15
	m.insertPage(1, now)
	if m.Song.Pages[1][0][0].Tie {
		t.Fatal("insertion connected across blank page")
	}
	if err := m.Song.validate(); err != nil {
		t.Fatal(err)
	}
}

func TestTieCrossPageExportAndLiveAudioMatch(t *testing.T) {
	song := defaultSong()
	song.Format = 3
	song.BPM = 120
	song.Pages = make([][steps][]Hit, 1)
	song.Pattern[15] = []Hit{{MIDI: 60}, {MIDI: 64}}
	song.Pages[0][0] = []Hit{{MIDI: 64, Tie: true}, {MIDI: 60, Tie: true}}
	song.Pages[0][1] = []Hit{{MIDI: 60, Tie: true}}
	path := filepath.Join(t.TempDir(), "cross.wav")
	if err := exportWAV(path, song); err != nil {
		t.Fatal(err)
	}
	wav := mpRead(t, path)
	now := time.Unix(100, 0)
	m := newModel(song)
	m.start(now)
	s := NewSynth()
	s.SetVolume(song.Volume)
	var pcm []int16
	for pos := 0; pos < 32; pos++ {
		for _, c := range m.tick(now.Add(time.Duration(pos) * m.stepDuration())) {
			applySound(s, c)
		}
		pcm = append(pcm, renderFrames(s, sampleRate/4)...)
	}
	s.AllOff()
	pcm = append(pcm, renderFrames(s, sampleRate)...)
	raw := make([]byte, len(pcm)*2)
	encodePCM(raw, pcm)
	if !bytes.Equal(wav[44:], raw) {
		t.Fatal("live/export differ across pages")
	}
}
