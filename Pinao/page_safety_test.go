package main

import (
	"testing"
	"time"
)

func TestPageClearConfirmationCannotCrossPlaybackBoundary(t *testing.T) {
	song := defaultSong()
	song.Format = 2
	song.Pages = make([][steps][]Hit, 1)
	song.Pattern[0] = []Hit{{MIDI: 60}}
	song.Pages[0][0] = []Hit{{MIDI: 72}}
	m := newModel(song)
	now := time.Unix(100, 0)
	m.startFrom(now, steps-1)
	m.tick(now)
	m.handle(keyEvent{Code: 111, Down: true}, now)
	// Input arrives before tick, so the model's last-rendered page is stale.
	later := now.Add(m.stepDuration())
	m.handle(keyEvent{Code: 111, Down: true}, later)
	if m.Page != 1 || len(m.Song.Pattern[0]) != 1 || len(m.Song.Pages[0][0]) != 1 {
		t.Fatal("confirmation crossed into another page or cleared the stale page")
	}
	m.handle(keyEvent{Code: 111, Down: true}, later.Add(time.Millisecond))
	if m.Song.pageCount() != 1 || m.Page != 0 || m.noteCount() != 0 {
		t.Fatal("confirmation did not reset the project to one empty page")
	}
}

func TestPageStopAtBoundaryKeepsActualPlaybackPage(t *testing.T) {
	song := defaultSong()
	song.Format = 2
	song.Pages = make([][steps][]Hit, 1)
	m := newModel(song)
	now := time.Unix(100, 0)
	m.startFrom(now, steps-1)
	m.tick(now)
	later := now.Add(m.stepDuration())
	m.handle(keyEvent{Code: 57, Down: true}, later)
	if m.Page != 1 || m.Playing || m.Step != -1 {
		t.Fatal("stop retained the stale page instead of the current playback page")
	}
	m.handle(keyEvent{Code: 57, Down: true}, later.Add(time.Second))
	if !m.Playing || m.Page != 1 || m.Step != 0 || m.StartStep != steps {
		t.Fatal("restart did not begin at the selected page's first step")
	}
}

func TestPageGestureSwitchesDeleteToSelectedCell(t *testing.T) {
	m := newModel(defaultSong())
	m.Song.Pattern[0] = []Hit{{MIDI: 60}}
	now := time.Unix(100, 0)
	m.handle(keyEvent{Code: 111, Down: true}, now)
	m.handle(keyEvent{Code: 24, Down: true}, now.Add(time.Millisecond))
	m.handle(keyEvent{Code: 24}, now.Add(50*time.Millisecond))
	m.handle(keyEvent{Code: 111, Down: true}, now.Add(100*time.Millisecond))
	if m.noteCount() != 0 || m.Song.pageCount() != 1 || m.Step != 0 || !m.StepMode {
		t.Fatal("Delete after page selection did not clear only the selected cell")
	}
}
