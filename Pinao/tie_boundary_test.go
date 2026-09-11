package main

import (
	"testing"
	"time"
)

func TestTieTempoOnUnprocessedBoundary(t *testing.T) {
	for _, code := range []uint16{103, 108} {
		m, now := tieModel()
		press(m, 30, now)
		press(m, 106, now)
		press(m, 106, now)
		m.start(now)
		m.tick(now)
		boundary := now.Add(m.stepDuration())
		press(m, code, boundary)
		out := m.tick(boundary)
		if out[0].Keep != 1 || out[1].Kind != "hold" {
			t.Fatal("tempo retriggered boundary", out)
		}
		m.start(now)
		m.tick(now)
		stalled := now.Add(2 * m.stepDuration())
		press(m, code, stalled)
		out = m.tick(stalled)
		if out[0].Keep != 0 || out[1].Kind != "on" {
			t.Fatal("skipped cells retained stale voice", out)
		}
	}
}

func TestTieTempoAfterLongPlayback(t *testing.T) {
	for _, code := range []uint16{103, 108} {
		for _, skipped := range []int{0, 2} {
			m, now := tieModel()
			m.Song.Format = 3
			m.Song.Pages = make([][steps][]Hit, 3)
			for pos := 0; pos < 4*steps; pos++ {
				m.Song.patternAt(pos / steps)[pos%steps] = []Hit{{MIDI: 60, Tie: pos > 0}}
			}
			m.start(now)
			oldDuration := m.stepDuration()
			for i := 0; i <= 44; i++ {
				m.tick(now.Add(time.Duration(i) * oldDuration))
			}
			boundary := now.Add(time.Duration(45+skipped) * oldDuration)
			press(m, code, boundary)
			out := m.tick(boundary)
			want := "hold"
			if skipped != 0 {
				want = "on"
			}
			if out[1].Kind != want {
				t.Fatalf("code=%d skipped=%d: %v", code, skipped, out)
			}
		}
	}
}

func TestTieLiveStealDoesNotCascade(t *testing.T) {
	for _, drum := range []bool{false, true} {
		m, now := tieModel()
		for i := 0; i < voiceCount; i++ {
			m.Song.Pattern[0] = append(m.Song.Pattern[0], Hit{MIDI: 60 + i})
			m.Song.Pattern[1] = append(m.Song.Pattern[1], Hit{MIDI: 60 + i, Tie: true})
		}
		m.Song.Format = 3
		m.start(now)
		s := NewSynth()
		for _, c := range m.tick(now) {
			applySound(s, c)
		}
		renderFrames(s, 1000)
		var original [8]uint64
		for _, v := range s.voices {
			original[v.id-300] = v.serial
		}
		if drum {
			s.Drum(200, 0)
		} else {
			s.NoteOn(30, 80, 0, 0.8)
		}
		before := s.serial
		for _, c := range m.tick(now.Add(m.stepDuration())) {
			applySound(s, c)
		}
		if s.serial != before+1 {
			t.Fatal("recovery cascaded", s.serial-before)
		}
		for _, v := range s.voices {
			if v.id < 300 || v.id > 307 || v.releasing {
				t.Fatal("invalid recovered chord")
			}
			if v.id != 300 && v.serial != original[v.id-300] {
				t.Fatal("surviving note reattacked")
			}
		}
	}
}

func TestTieNewAttackProtectsSurvivingChord(t *testing.T) {
	s := NewSynth()
	for i := 0; i < 8; i++ {
		s.NoteOn(300+i, 60+i, 0, 0.8)
	}
	s.NoteOn(30, 80, 0, 0.8) // Replace the oldest sequence voice.
	// Next cell has a new attack in missing slot 0 and ties in slots 1..7.
	applySound(s, soundCommand{Kind: "loop-off", Keep: 254})
	before := s.serial
	applySound(s, soundCommand{Kind: "on", ID: 300, MIDI: 72})
	for i := 1; i < 8; i++ {
		applySound(s, soundCommand{Kind: "hold", ID: 300 + i, MIDI: 60 + i})
	}
	if s.serial != before+1 {
		t.Fatal("new note stole a protected tie")
	}
}

func TestTieStopAndResumeRearticulates(t *testing.T) {
	m, now := tieModel()
	press(m, 30, now)
	press(m, 106, now)
	m.handle(keyEvent{Code: 30}, now)
	m.start(now)
	s := NewSynth()
	for _, c := range m.tick(now) {
		applySound(s, c)
	}
	for _, c := range m.tick(now.Add(m.stepDuration())) {
		applySound(s, c)
	}
	for _, c := range press(m, 57, now.Add(m.stepDuration())) {
		applySound(s, c)
	}
	renderFrames(s, releaseSamples+1)
	if s.Active() {
		t.Fatal("paused note stuck")
	}
	press(m, 57, now.Add(time.Second))
	for _, c := range m.tick(now.Add(time.Second)) {
		applySound(s, c)
	}
	if !s.Active() {
		t.Fatal("resume from tie silent")
	}
}
