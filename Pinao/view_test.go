package main

import (
	"testing"
	"time"
)

func TestRefreshUnchangedBackgroundDoesNotAllocate(t *testing.T) {
	m := newModel(defaultSong())
	now := time.Unix(100, 0)
	r := newRefreshScheduler(render(m, now), now)
	calls := 0
	submit := func(frame) { calls++ }
	// Prime the visual cache before measuring steady-state work.
	r.update(m, now, true, submit)
	allocs := testing.AllocsPerRun(1000, func() {
		now = now.Add(backgroundRefreshInterval)
		r.update(m, now, false, submit)
	})
	if allocs != 0 || calls != 0 {
		t.Fatalf("idle work allocated %g objects and submitted %d frames", allocs, calls)
	}
}

func TestRefreshHiddenLoopDoesNotRedrawHelp(t *testing.T) {
	m := newModel(demoSong())
	now := time.Unix(100, 0)
	m.start(now)
	m.Help = true
	initial := render(m, now)
	r := newRefreshScheduler(initial, now)
	calls := 0
	submit := func(frame) { calls++ }
	for i := 1; i <= 50; i++ {
		at := now.Add(time.Duration(i) * backgroundRefreshInterval)
		m.tick(at)
		r.update(m, at, false, submit)
	}
	if calls != 0 {
		t.Fatalf("help redrawn %d times for hidden loop activity", calls)
	}
	m.Help = false
	r.update(m, now.Add(13*time.Second), true, submit)
	if calls != 1 {
		t.Fatal("closing help did not immediately show current playback")
	}
}

func TestRefreshVisibleChangesAreNotLostByCache(t *testing.T) {
	tests := []struct {
		name   string
		change func(*model, time.Time)
	}{
		{"tone", func(m *model, _ time.Time) { m.Song.Tone++ }},
		{"tempo", func(m *model, _ time.Time) { m.Song.BPM++ }},
		{"octave", func(m *model, _ time.Time) { m.Song.Octave++ }},
		{"volume", func(m *model, _ time.Time) { m.Song.Volume++ }},
		{"play", func(m *model, now time.Time) { m.start(now) }},
		{"record", func(m *model, _ time.Time) { m.Recording = true }},
		{"pattern", func(m *model, _ time.Time) { m.Song.Pattern[15] = []Hit{{MIDI: 60}} }},
		{"notice", func(m *model, now time.Time) { m.message("SAVED", now) }},
		{"dirty", func(m *model, now time.Time) { m.changed(now) }},
		{"help", func(m *model, _ time.Time) { m.Help = true }},
		{"key", func(m *model, now time.Time) { m.handle(keyEvent{Code: 17, Down: true}, now) }},
		{"drum", func(m *model, now time.Time) { m.handle(keyEvent{Code: 46, Down: true}, now) }},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			m := newModel(defaultSong())
			now := time.Unix(100, 0)
			initial := render(m, now)
			r := newRefreshScheduler(initial, now)
			calls := 0
			var got frame
			submit := func(f frame) { calls++; got = f }
			r.update(m, now, true, submit)
			at := now.Add(time.Millisecond)
			tt.change(m, at)
			r.update(m, at, true, submit)
			if calls != 1 || got == initial || got != render(m, at) {
				t.Fatal("visual cache lost or delayed visible change")
			}
		})
	}
}

func TestRefreshReleasedChordSurvivesPendingFrameReplacement(t *testing.T) {
	m := newModel(defaultSong())
	now := time.Unix(100, 0)
	r := newRefreshScheduler(render(m, now), now)
	var latest frame
	// This slot simulates the writer coalescing updates while a prior write is busy.
	submit := func(f frame) { latest = f }
	for i, code := range []uint16{30, 32, 34} {
		at := now.Add(time.Duration(i) * 20 * time.Millisecond)
		m.handle(keyEvent{Code: code, Down: true}, at)
		r.update(m, at, true, submit)
		m.handle(keyEvent{Code: code}, at.Add(time.Millisecond))
		r.update(m, at.Add(time.Millisecond), true, submit)
	}
	at := now.Add(750 * time.Millisecond)
	m.tick(at)
	r.update(m, at, false, submit)
	v := m.view(at)
	if v.Keys != (1<<0|1<<4|1<<7) || latest != renderView(v) || len(m.Held) != 0 {
		t.Fatal("coalesced frame lost short chord feedback or retained audio holds")
	}
}

func TestViewRejectsInvalidActivityIndices(t *testing.T) {
	m := newModel(defaultSong())
	now := time.Unix(100, 0)
	for _, key := range []int{-1, 13, 999} {
		m.addSpark(key, false, now)
		m.addSpark(key, true, now)
	}
	v := m.view(now)
	if v.Notes != 0 || v.Drums != 0 {
		t.Fatal("out-of-range activity highlighted an unrelated key")
	}
}
