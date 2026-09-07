package main

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"
	"time"

	"c1device"
)

func TestVisualModeKeyAndRepeat(t *testing.T) {
	for _, key := range []rune{'v', 'V'} {
		if musicActionForEvent(c1device.Event{Key: c1device.KeyRune, Rune: key}) != actionCycleVisual {
			t.Fatal("V does not cycle visual")
		}
		if musicActionForEvent(c1device.Event{Key: c1device.KeyRune, Rune: key, Repeat: true}) != actionNone {
			t.Fatal("V repeats should be ignored")
		}
	}
	mode := visualStatic
	for i := 0; i < int(visualModeCount); i++ {
		mode = mode.next()
	}
	if mode != visualStatic {
		t.Fatal("visual cycle never returns to static")
	}
}

func TestVisualSettingsRoundTripAndInvalidFallback(t *testing.T) {
	path := filepath.Join(t.TempDir(), "music", "display.json")
	if loadVisualMode(path) != visualStatic {
		t.Fatal("missing file should select static")
	}
	for _, mode := range []visualMode{visualBars, visualRecord, visualStatic} {
		if err := saveVisualMode(path, mode); err != nil {
			t.Fatal(err)
		}
		if loadVisualMode(path) != mode {
			t.Fatal("mode was not persisted")
		}
	}
	for _, data := range []string{"not json", `{"visual":255}`, `{"visual":-1}`, `{"visual":"bars"}`} {
		if err := os.WriteFile(path, []byte(data), 0600); err != nil {
			t.Fatal(err)
		}
		if loadVisualMode(path) != visualStatic {
			t.Fatal("invalid settings should be static")
		}
	}
	if saveVisualMode(path, visualModeCount) == nil {
		t.Fatal("invalid mode accepted")
	}
}

func TestProgressRefreshBudget(t *testing.T) {
	last := time.Unix(100, 0)
	for _, mode := range []visualMode{visualStatic, visualBars, visualRecord} {
		state := appState{visual: mode, playing: true}
		interval := progressInterval(mode)
		if shouldDrawProgress(state, last, last.Add(interval-time.Millisecond)) {
			t.Fatal("refresh too early")
		}
		if !shouldDrawProgress(state, last, last.Add(interval)) {
			t.Fatal("refresh never happens")
		}
		state.paused = true
		if shouldDrawProgress(state, last, last.Add(time.Hour)) {
			t.Fatal("paused playback causes refresh")
		}
		state.paused, state.playing = false, false
		if shouldDrawProgress(state, last, last.Add(time.Hour)) {
			t.Fatal("idle playback causes refresh")
		}
	}
	if progressInterval(visualStatic) < 10*time.Second {
		t.Fatal("static mode refreshes too often")
	}
	if motionRefreshInterval != 300*time.Millisecond {
		t.Fatal("motion cadence must stay at the bounded fast-refresh target")
	}
	if progressInterval(visualBars) != time.Second {
		t.Fatal("text refresh must be decoupled from fast cover frames")
	}
}

func TestDecorativeMotionChangesOnlyWhenRequested(t *testing.T) {
	if bytes.Equal(barsArtwork(0, true).Pix, barsArtwork(1, true).Pix) {
		t.Fatal("bars do not move")
	}
	if bytes.Equal(recordArtwork(0, true).Pix, recordArtwork(1, true).Pix) {
		t.Fatal("record does not move")
	}
	if !bytes.Equal(barsArtwork(0, false).Pix, barsArtwork(8, false).Pix) {
		t.Fatal("idle bars move")
	}
	if !bytes.Equal(recordArtwork(0, false).Pix, recordArtwork(8, false).Pix) {
		t.Fatal("static cover moves")
	}
	state := appState{playing: true, visual: visualBars, visualTick: 1}
	phase := visualPhase(state)
	state.visualTick = 2
	if visualPhase(state) == phase {
		t.Fatal("visual timer does not advance animation")
	}
	phase = visualPhase(state)
	state.paused = true
	if visualPhase(state) != phase {
		t.Fatal("pausing should freeze record, not reset it")
	}
	for _, artwork := range [][]byte{recordArtwork(0, false).Pix, barsArtwork(1, true).Pix} {
		for _, value := range artwork {
			if value != 0 && value != 255 {
				t.Fatal("artwork is not one-bit")
			}
		}
	}
}
