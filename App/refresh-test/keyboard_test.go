package main

import (
	"testing"
	"time"
)

func TestLetterPresets(t *testing.T) {
	for i, key := range []uint16{16, 17, 18, 19, 20, 21} {
		for _, paused := range []bool{false, true} {
			s := state{interval: 1500 * time.Millisecond, paused: paused, mode: 2}
			if got := s.key(key); got != newSegment {
				t.Fatalf("%c did not select a preset: %v", "QWERTY"[i], got)
			}
			if s.interval != presets[i] || s.paused != paused || s.mode != 2 {
				t.Fatalf("%c changed unexpected state: %+v", "QWERTY"[i], s)
			}
			if s.key(key) != noAction {
				t.Fatal("unchanged preset triggered another full refresh")
			}
		}
	}
}

func TestLetterControlsDoNotConflict(t *testing.T) {
	s := state{interval: 700 * time.Millisecond}
	if s.key(16) == quit || s.interval != time.Second {
		t.Fatal("Q still exits")
	}
	if s.key(19) != newSegment || s.interval != 300*time.Millisecond {
		t.Fatal("R did not select 300ms")
	}
	if s.key(46) != newSegment || s.interval != 300*time.Millisecond {
		t.Fatal("C clean changed speed")
	}
	s.key(35) // H opens help, pauses.
	for _, key := range []uint16{16, 17, 18, 19, 20, 21} {
		if s.key(key) != noAction || s.interval != 300*time.Millisecond {
			t.Fatal("help did not block speed controls")
		}
	}
	if s.key(45) != quit {
		t.Fatal("X cannot exit help")
	}
}
